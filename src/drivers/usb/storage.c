/*
 * USB mass storage: USB sticks, card readers, external disks. After Linux's
 * drivers/usb/storage:
 *
 *   transport.c   Bulk-Only Transport: a 31-byte command block (CBW) out,
 *                 the data, a 13-byte status (CSW) back; stalls cleared with
 *                 CLEAR_FEATURE(HALT), the reset recovery for anything worse
 *                 (usb_stor_Bulk_transport, usb_stor_Bulk_reset)
 *   scsiglue.c    the settings Linux uses for USB disks: MODE SENSE page 0x3F
 *                 with 192 bytes, READ CAPACITY(10) first, at most 240
 *                 sectors per command
 *   drivers/scsi/sd.c   the SCSI disk commands themselves
 *
 * Only Bulk-Only (not UAS: UAS drives offer Bulk-Only as alternate setting 0).
 * Each logical unit (a card reader's slots) that has a medium becomes a disk.
 */

#include "drivers/usb/usb.h"
#include "block/block.h"
#include "lib/errno.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "sched/sched.h"

#define BUFFER_PAGES   16                  /* 64 KiB of DMA memory for data */
#define MAX_TRANSFER   (BUFFER_PAGES * 4096)
#define MAX_LUNS       4
#define TIMEOUT_MS     20000

/* Bulk-Only Transport (transport.h) */
#define CBW_SIGNATURE  0x43425355   /* "USBC" */
#define CSW_SIGNATURE  0x53425355   /* "USBS" */
#define CBW_LEN        31
#define CSW_LEN        13
#define REQ_RESET      0xFF
#define REQ_MAX_LUN    0xFE

/* SCSI */
#define TEST_UNIT_READY  0x00
#define REQUEST_SENSE    0x03
#define INQUIRY          0x12
#define MODE_SENSE_6     0x1A
#define READ_CAPACITY_10 0x25
#define READ_10          0x28
#define WRITE_10         0x2A
#define SYNC_CACHE_10    0x35
#define MODE_SENSE_10    0x5A
#define READ_16          0x88
#define WRITE_16         0x8A
#define SERVICE_IN_16    0x9E

enum { XFER_GOOD, XFER_FAILED, XFER_ERROR };   /* command passed / failed (sense) / transport broke */

struct storage {
    struct usb_device *dev;
    uint8_t iface, subclass;
    uint8_t ep_in, ep_out;       /* endpoint addresses */
    int dci_in, dci_out;
    uint32_t tag;
    bool gone;
    void *cmd;                   /* one page for CBW/CSW */
    uint64_t cmd_phys;
    uint8_t *data;               /* BUFFER_PAGES pages for data */
    uint64_t data_phys;
    uint32_t max_sectors;        /* per command */
    int luns;
    struct lun {
        struct storage *s;
        int lun;
        struct block_device bdev;
        uint8_t sense_key, asc, ascq;
    } lun[MAX_LUNS];
};

static int disk_number;

/* --- Bulk-Only Transport --------------------------------------------------------------------------------- */

static void clear_halt(struct storage *s, uint8_t ep_addr, int dci) {
    xhci_reset_endpoint(s->dev, dci);
    xhci_control(s->dev, 0x02, 1 /* CLEAR_FEATURE */, 0 /* ENDPOINT_HALT */, ep_addr, NULL, 0);
}

/* usb_stor_Bulk_reset, then clear both halts: the device is back to waiting for a command. */
static void reset_recovery(struct storage *s) {
    xhci_control(s->dev, 0x21, REQ_RESET, 0, s->iface, NULL, 0);
    sched_sleep_ms(50);
    clear_halt(s, s->ep_in, s->dci_in);
    clear_halt(s, s->ep_out, s->dci_out);
}

/* One command: CBW, data (in or out of s->data), CSW. Returns XFER_*. */
static int transport(struct storage *s, int lun, const uint8_t *cdb, int cdb_len, bool in, uint32_t length,
                     uint32_t *done) {
    uint8_t *cbw = s->cmd;
    memset(cbw, 0, CBW_LEN);
    uint32_t tag = ++s->tag;
    cbw[0] = 0x55; cbw[1] = 0x53; cbw[2] = 0x42; cbw[3] = 0x43;   /* "USBC" */
    memcpy(cbw + 4, &tag, 4);
    memcpy(cbw + 8, &length, 4);
    cbw[12] = in ? 0x80 : 0x00;
    cbw[13] = (uint8_t)lun;
    cbw[14] = (uint8_t)cdb_len;
    memcpy(cbw + 15, cdb, (size_t)cdb_len);
    uint32_t moved = 0;
    int r = xhci_bulk(s->dev, s->dci_out, s->cmd_phys, CBW_LEN, &moved, TIMEOUT_MS);
    if (r != 0 || moved != CBW_LEN) {
        if (r == USB_STALL) {
            clear_halt(s, s->ep_out, s->dci_out);
        }
        reset_recovery(s);
        return XFER_ERROR;
    }

    /* Data. A stall here ends the data stage: clear it and go on to the status. */
    uint32_t got = 0;
    if (length) {
        int dci = in ? s->dci_in : s->dci_out;
        r = xhci_bulk(s->dev, dci, s->data_phys, length, &got, TIMEOUT_MS);
        if (r == USB_STALL) {
            clear_halt(s, in ? s->ep_in : s->ep_out, dci);
        } else if (r != 0) {
            reset_recovery(s);
            return XFER_ERROR;
        }
    }
    if (done) {
        *done = got;
    }

    /* Status: retry once after a stall, and once after a zero-length packet (broken devices). */
    uint8_t *csw = (uint8_t *)s->cmd + 64;
    uint64_t csw_phys = s->cmd_phys + 64;
    for (int attempt = 0; attempt < 3; attempt++) {
        r = xhci_bulk(s->dev, s->dci_in, csw_phys, CSW_LEN, &moved, TIMEOUT_MS);
        if (r == USB_STALL) {
            clear_halt(s, s->ep_in, s->dci_in);
            continue;
        }
        if (r == 0 && moved == 0) {
            continue;
        }
        break;
    }
    uint32_t sig, csw_tag;
    memcpy(&sig, csw, 4);
    memcpy(&csw_tag, csw + 4, 4);
    if (r != 0 || moved != CSW_LEN || sig != CSW_SIGNATURE || csw_tag != tag || csw[12] > 2) {
        reset_recovery(s);
        return XFER_ERROR;
    }
    if (csw[12] == 2) {   /* phase error */
        reset_recovery(s);
        return XFER_ERROR;
    }
    return csw[12] == 0 ? XFER_GOOD : XFER_FAILED;
}

/* --- SCSI ---------------------------------------------------------------------------------------------- */

static void request_sense(struct lun *l) {
    uint8_t cdb[6] = { REQUEST_SENSE, 0, 0, 0, 18, 0 };
    uint32_t got = 0;
    l->sense_key = l->asc = l->ascq = 0;
    if (transport(l->s, l->lun, cdb, 6, true, 18, &got) == XFER_GOOD && got >= 14) {
        l->sense_key = l->s->data[2] & 0x0F;
        l->asc = l->s->data[12];
        l->ascq = l->s->data[13];
    }
}

/* A command with sense handling: a failed one fetches the sense data. 0 or -EIO. */
static int scsi(struct lun *l, const uint8_t *cdb, int len, bool in, uint32_t bytes, uint32_t *got) {
    int r = transport(l->s, l->lun, cdb, len, in, bytes, got);
    if (r == XFER_FAILED) {
        request_sense(l);
    }
    return r == XFER_GOOD ? 0 : -EIO;
}

static bool test_unit_ready(struct lun *l) {
    /* sd_spinup_disk-ish: unit attentions after power-on, "becoming ready", give it a few seconds. */
    for (int i = 0; i < 20; i++) {
        uint8_t cdb[6] = { TEST_UNIT_READY };
        if (scsi(l, cdb, 6, false, 0, NULL) == 0) {
            return true;
        }
        if (l->sense_key == 0x02 && l->asc == 0x3A) {
            return false;   /* no medium (an empty card slot) */
        }
        sched_sleep_ms(l->sense_key == 0x06 ? 10 : 250);
    }
    return false;
}

static bool read_capacity(struct lun *l, uint64_t *sectors, uint32_t *size) {
    uint8_t cdb10[10] = { READ_CAPACITY_10 };
    uint32_t got = 0;
    for (int i = 0; i < 3; i++) {
        if (scsi(l, cdb10, 10, true, 8, &got) == 0 && got >= 8) {
            break;
        }
        got = 0;
    }
    if (got < 8) {
        return false;
    }
    const uint8_t *d = l->s->data;
    uint32_t last = (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3];
    *size = (uint32_t)d[4] << 24 | (uint32_t)d[5] << 16 | (uint32_t)d[6] << 8 | d[7];
    *sectors = (uint64_t)last + 1;
    if (last == 0xFFFFFFFF) {   /* more than 2 TiB: READ CAPACITY(16) */
        uint8_t cdb16[16] = { SERVICE_IN_16, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0 };
        if (scsi(l, cdb16, 16, true, 32, &got) != 0 || got < 12) {
            return false;
        }
        uint64_t last64 = 0;
        for (int i = 0; i < 8; i++) {
            last64 = last64 << 8 | d[i];
        }
        *size = (uint32_t)d[8] << 24 | (uint32_t)d[9] << 16 | (uint32_t)d[10] << 8 | d[11];
        *sectors = last64 + 1;
    }
    return *size >= 512 && *size <= 4096 && !(*size & (*size - 1));
}

/* sd_read_write_protect_flag: MODE SENSE, all pages, 192 bytes (what Windows asks for). */
static bool write_protected(struct lun *l) {
    uint32_t got = 0;
    if (l->s->subclass == 0x06) {
        uint8_t cdb[6] = { MODE_SENSE_6, 0, 0x3F, 0, 192, 0 };
        if (scsi(l, cdb, 6, true, 192, &got) == 0 && got >= 4) {
            return l->s->data[2] & 0x80;
        }
    } else {
        uint8_t cdb[10] = { MODE_SENSE_10, 0, 0x3F, 0, 0, 0, 0, 0, 192, 0 };
        if (scsi(l, cdb, 10, true, 192, &got) == 0 && got >= 8) {
            return l->s->data[3] & 0x80;
        }
    }
    return false;   /* didn't answer: assume writable, like Linux */
}

static void rw_cdb(uint8_t *cdb, int *len, bool write, uint64_t lba, uint32_t count) {
    memset(cdb, 0, 16);
    if (lba + count <= 0xFFFFFFFFull) {
        cdb[0] = write ? WRITE_10 : READ_10;
        cdb[2] = (uint8_t)(lba >> 24);
        cdb[3] = (uint8_t)(lba >> 16);
        cdb[4] = (uint8_t)(lba >> 8);
        cdb[5] = (uint8_t)lba;
        cdb[7] = (uint8_t)(count >> 8);
        cdb[8] = (uint8_t)count;
        *len = 10;
    } else {
        cdb[0] = write ? WRITE_16 : READ_16;
        for (int i = 0; i < 8; i++) {
            cdb[2 + i] = (uint8_t)(lba >> (56 - 8 * i));
        }
        cdb[10] = (uint8_t)(count >> 24);
        cdb[11] = (uint8_t)(count >> 16);
        cdb[12] = (uint8_t)(count >> 8);
        cdb[13] = (uint8_t)count;
        *len = 16;
    }
}

/* --- block device -------------------------------------------------------------------------------------- */

static int rw(struct block_device *d, uint64_t lba, uint32_t count, uint8_t *buf, bool write) {
    struct lun *l = d->priv;
    struct storage *s = l->s;
    uint32_t ss = d->sector_size;
    int err = 0;
    usb_lock();
    while (count && !err) {
        if (s->gone) {
            err = -EIO;
            break;
        }
        uint32_t n = count < s->max_sectors ? count : s->max_sectors;
        if (n > MAX_TRANSFER / ss) {
            n = MAX_TRANSFER / ss;
        }
        uint8_t cdb[16];
        int len;
        rw_cdb(cdb, &len, write, lba, n);
        uint32_t bytes = n * ss, got = 0;
        if (write) {
            memcpy(s->data, buf, bytes);
        }
        /* A couple of retries: unit attentions, the odd transport hiccup. */
        err = -EIO;
        for (int attempt = 0; attempt < 3 && err && !s->gone; attempt++) {
            err = scsi(l, cdb, len, !write, bytes, &got);
            if (!err && got != bytes) {
                err = -EIO;
            }
        }
        if (err) {
            kprintf("[WARN] %s: %s of %u sectors at %lu failed (sense %x/%02x/%02x)\n", d->name,
                    write ? "write" : "read", n, lba, l->sense_key, l->asc, l->ascq);
            break;
        }
        if (!write) {
            memcpy(buf, s->data, bytes);
        }
        lba += n;
        count -= n;
        buf += bytes;
    }
    usb_unlock();
    return err;
}

static int storage_read(struct block_device *d, uint64_t lba, uint32_t count, void *buf) {
    return rw(d, lba, count, buf, false);
}

static int storage_write(struct block_device *d, uint64_t lba, uint32_t count, const void *buf) {
    return rw(d, lba, count, (uint8_t *)buf, true);
}

static int storage_flush(struct block_device *d) {
    struct lun *l = d->priv;
    usb_lock();
    if (!l->s->gone) {
        uint8_t cdb[10] = { SYNC_CACHE_10 };
        scsi(l, cdb, 10, false, 0, NULL);   /* (many sticks don't know it: that's fine) */
    }
    usb_unlock();
    return l->s->gone ? -EIO : 0;
}

/* --- probing ------------------------------------------------------------------------------------------- */

static void trim_copy(char *out, const uint8_t *in, int len) {
    while (len && (in[len - 1] == ' ' || in[len - 1] == 0)) {
        len--;
    }
    int start = 0;
    while (start < len && in[start] == ' ') {
        start++;
    }
    int o = 0;
    for (int i = start; i < len; i++) {
        out[o++] = in[i] >= 0x20 && in[i] < 0x7F ? (char)in[i] : '?';
    }
    out[o] = '\0';
}

/* Names that say nothing about the drive. */
static bool generic(const char *name) {
    static const char *const words[] = { "Generic", "USB", "Mass Storage", "Mass-Storage", "Disk", "External",
                                         "USB3.0", "USB 3.0", "Storage Device", "SCSI", "Flash Disk" };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (strcmp(name, words[i]) == 0) {
            return true;
        }
    }
    return !name[0];
}

/* ATA strings are byte-swapped in 16-bit words and padded with spaces. */
static void ata_string(const uint8_t *id, int first_word, int words, char *out, size_t size) {
    size_t o = 0;
    for (int w = first_word; w < first_word + words && o + 2 < size; w++) {
        out[o++] = (char)id[2 * w + 1];
        out[o++] = (char)id[2 * w];
    }
    while (o && (out[o - 1] == ' ' || out[o - 1] == 0)) {
        o--;
    }
    out[o] = '\0';
    size_t start = 0;
    while (out[start] == ' ') {
        start++;
    }
    memmove(out, out + start, o - start + 1);
    for (char *c = out; *c; c++) {
        if ((unsigned char)*c < 0x20 || (unsigned char)*c >= 0x7F) {
            *c = '?';
        }
    }
}

/* IDENTIFY DEVICE through SCSI/ATA Translation: ATA PASS-THROUGH(16), PIO data-in, one sector
   (what smartctl/hdparm do with SATA-to-USB bridges). */
static bool ata_identify(struct lun *l, char *model, size_t msize, char *serial, size_t ssize) {
    uint8_t cdb[16] = { 0x85, 4 << 1, 0x0E, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0xEC, 0 };
    uint32_t got = 0;
    if (scsi(l, cdb, 16, true, 512, &got) != 0 || got < 512) {
        return false;
    }
    const uint8_t *id = l->s->data;
    ata_string(id, 27, 20, model, msize);
    ata_string(id, 10, 10, serial, ssize);
    /* A bridge that answers with garbage (or zeros) isn't telling us anything. */
    for (const char *c = model; *c; c++) {
        if (*c == '?') {
            return false;
        }
    }
    return model[0] != '\0';
}

static bool setup_lun(struct storage *s, struct lun *l) {
    uint8_t cdb[6] = { INQUIRY, 0, 0, 0, 36, 0 };
    uint32_t got = 0;
    if (scsi(l, cdb, 6, true, 36, &got) != 0 || got < 36) {
        return false;
    }
    uint8_t type = s->data[0] & 0x1F, qualifier = s->data[0] >> 5;
    if (qualifier != 0 || (type != 0x00 && type != 0x0E)) {
        return false;   /* not a disk (a CD drive, or nothing on this LUN) */
    }
    bool removable = s->data[1] & 0x80;
    char vendor[9], product[17];
    trim_copy(vendor, s->data + 8, 8);
    trim_copy(product, s->data + 16, 16);

    if (!test_unit_ready(l)) {
        usb_log("  %s %s: no medium", vendor, product);
        return false;
    }
    uint64_t sectors;
    uint32_t size;
    if (!read_capacity(l, &sectors, &size)) {
        usb_warn("%s %s: couldn't read its capacity", vendor, product);
        return false;
    }
    struct block_device *d = &l->bdev;
    ksnprintf(d->name, sizeof(d->name), "usb%d", disk_number++);
    /* The name: the drive's own (ATA IDENTIFY through a SATA bridge), else INQUIRY's, else the USB
       product string; INQUIRY from enclosures is often the bridge chip's ("JMicron Generic"). */
    char ata_model[48], ata_serial[24], usb_product[64], usb_maker[64];
    bool ata = !removable && ata_identify(l, ata_model, sizeof(ata_model), ata_serial, sizeof(ata_serial));
    usb_string(s->dev, s->dev->str_product, usb_product, sizeof(usb_product));
    usb_string(s->dev, s->dev->str_manufacturer, usb_maker, sizeof(usb_maker));
    if (ata) {
        strlcpy(d->model, ata_model, sizeof(d->model));
    } else if (vendor[0] && product[0] && !generic(vendor) && !generic(product)) {
        ksnprintf(d->model, sizeof(d->model), "%s %s", vendor, product);
    } else if (usb_product[0] && !generic(usb_product)) {
        if (usb_maker[0] && !generic(usb_maker) && strncmp(usb_product, usb_maker, strlen(usb_maker)) != 0) {
            ksnprintf(d->model, sizeof(d->model), "%s %s", usb_maker, usb_product);
        } else {
            strlcpy(d->model, usb_product, sizeof(d->model));
        }
    } else if (vendor[0] && product[0]) {
        ksnprintf(d->model, sizeof(d->model), "%s %s", vendor, product);
    } else {
        ksnprintf(d->model, sizeof(d->model), "%s", product[0] ? product : vendor[0] ? vendor : "USB drive");
    }
    if (ata && ata_serial[0]) {
        strlcpy(d->serial, ata_serial, sizeof(d->serial));
    } else {
        usb_string(s->dev, s->dev->str_serial, d->serial, sizeof(d->serial));
    }
    d->sector_size = size;
    d->sectors = sectors;
    d->removable = removable;
    d->external = true;
    d->read_only = write_protected(l);
    d->read = storage_read;
    d->write = storage_write;
    d->flush = storage_flush;
    d->priv = l;
    return true;
}

bool storage_probe(struct usb_device *dev, const uint8_t *config, int length) {
    /* An interface (alternate setting 0) of class 08 (mass storage), protocol 50 (Bulk-Only). */
    int iface = -1, subclass = 0;
    uint8_t in = 0, out = 0, in_burst = 0, out_burst = 0;
    uint16_t in_mps = 0, out_mps = 0;
    uint8_t *last_burst = NULL;
    for (int at = 0; at + 2 <= length && config[at] >= 2; at += config[at]) {
        const uint8_t *d = config + at;
        if (d[1] == 4 && config[at] >= 9) {   /* interface */
            if (iface >= 0 && in && out) {
                break;   /* already have one */
            }
            bool bot = d[3] == 0 && d[5] == 0x08 && d[7] == 0x50;
            iface = bot ? d[2] : -1;
            subclass = d[6];
            in = out = 0;
            last_burst = NULL;
        } else if (d[1] == 5 && config[at] >= 7 && iface >= 0 && (d[3] & 3) == 2) {   /* bulk endpoint */
            uint16_t mps = (uint16_t)(d[4] | d[5] << 8);
            if (d[2] & 0x80) {
                in = d[2];
                in_mps = mps;
                last_burst = &in_burst;
            } else {
                out = d[2];
                out_mps = mps;
                last_burst = &out_burst;
            }
        } else if (d[1] == 0x30 && config[at] >= 6 && last_burst) {   /* SuperSpeed companion */
            *last_burst = d[2];
        }
    }
    if (iface < 0 || !in || !out) {
        return false;
    }

    struct storage *s = kzalloc(sizeof(*s));
    if (!s) {
        return true;
    }
    s->dev = dev;
    s->iface = (uint8_t)iface;
    s->subclass = (uint8_t)subclass;
    s->ep_in = in;
    s->ep_out = out;
    /* scsiglue.c: 240 sectors (Linux's default for usb-storage), 2048 on SuperSpeed; our buffer is 64 KiB. */
    s->max_sectors = dev->speed == USB_SPEED_SUPER ? 2048 : 240;
    void *v;
    s->cmd_phys = xhci_dma_alloc(dev, 1, &v);
    s->cmd = v;
    s->data_phys = xhci_dma_alloc(dev, BUFFER_PAGES, &v);
    s->data = v;
    if (!s->cmd_phys || !s->data_phys ||
        !xhci_add_bulk(dev, in, in_mps, in_burst, out, out_mps, out_burst, &s->dci_in, &s->dci_out)) {
        usb_warn("%04x:%04x: couldn't set up the storage endpoints", dev->vendor, dev->product);
        xhci_dma_free(s->cmd_phys, 1);
        xhci_dma_free(s->data_phys, BUFFER_PAGES);
        kfree(s);
        return true;
    }
    dev->storage = s;

    /* usb_stor_Bulk_max_lun: devices that don't like it stall or say nothing; that means one LUN. */
    uint8_t max_lun = 0;
    if (xhci_control(dev, 0xA1, REQ_MAX_LUN, 0, s->iface, &max_lun, 1) != 1 || max_lun > 15) {
        max_lun = 0;
    }
    s->luns = max_lun + 1 > MAX_LUNS ? MAX_LUNS : max_lun + 1;

    for (int i = 0; i < s->luns; i++) {
        struct lun *l = &s->lun[i];
        l->s = s;
        l->lun = i;
        if (setup_lun(s, l)) {
            usb_note_disk();
            block_add_disk(&l->bdev);
        } else {
            l->bdev.sectors = 0;   /* (not registered) */
        }
    }
    return true;
}

void storage_remove(struct usb_device *dev) {
    struct storage *s = dev->storage;
    if (!s) {
        return;
    }
    /* Called with the USB lock held, so no transfer is running: the DMA memory can go now.
       The struct stays: the block devices point into it. */
    s->gone = true;
    for (int i = 0; i < s->luns; i++) {
        if (s->lun[i].bdev.sectors) {
            block_disk_gone(&s->lun[i].bdev);
        }
    }
    xhci_dma_free(s->cmd_phys, 1);
    xhci_dma_free(s->data_phys, BUFFER_PAGES);
    s->cmd = s->data = NULL;
    dev->storage = NULL;
}
