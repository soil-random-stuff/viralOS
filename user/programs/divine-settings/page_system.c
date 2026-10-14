/* General (about this computer, its name), Displays, Date & Time and Storage. */

#include <config.h>
#include <image.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "settings.h"
#include <paths.h>

#define HOSTNAME_FILE _PATH_HOSTNAME
#define OS_RELEASE    _PATH_OS_RELEASE

/* --- General ------------------------------------------------------------------------------------- */

static struct field hostname;
static char cpu_name[64] = "Unknown";
static char os_version[32] = "";
static struct os_sysinfo sys;

static void cpuid(uint32_t leaf, uint32_t r[4]) {
    __asm__ volatile ("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0));
}

/* The processor's name from CPUID 0x80000002-4 ("Intel(R) Core(TM) i5-8500 CPU @ 3.00GHz"). */
static void read_cpu_name(void) {
    uint32_t r[4];
    cpuid(0x80000000, r);
    if (r[0] < 0x80000004) {
        return;
    }
    char name[49];
    for (uint32_t i = 0; i < 3; i++) {
        cpuid(0x80000002 + i, r);
        memcpy(name + i * 16, r, 16);
    }
    name[48] = '\0';
    char *p = name;
    while (*p == ' ') {
        p++;
    }
    strlcpy(cpu_name, p, sizeof(cpu_name));
}

static void read_hostname(void) {
    char name[64] = "viralos";
    int fd = open(HOSTNAME_FILE, O_RDONLY);
    if (fd >= 0) {
        long n = read(fd, name, sizeof(name) - 1);
        close(fd);
        name[n > 0 ? n : 0] = '\0';
        for (char *c = name; *c; c++) {
            if (*c == '\n' || *c == '\r' || *c == ' ') {
                *c = '\0';
                break;
            }
        }
    }
    field_set(&hostname, name);
}

static void save_hostname(void) {
    char name[64];
    int n = 0;
    for (const char *c = hostname.text; *c && n < 63; c++) {
        bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-';
        if (ok) {
            name[n++] = *c;
        }
    }
    name[n] = '\0';
    if (!n) {
        settings_message(GUI_ICON_ERROR, "Not a computer name", "Use letters, digits and dashes.");
        return;
    }
    int fd = open(HOSTNAME_FILE, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        dprintf(fd, "%s\n", name);
        close(fd);
    }
    field_set(&hostname, name);
    ui.focus = NULL;
}

void page_general_show(void) {
    sysinfo(&sys);
    read_cpu_name();
    read_hostname();
    if (!config_get(OS_RELEASE, "VERSION", os_version, sizeof(os_version))) {
        strlcpy(os_version, "0.6", sizeof(os_version));
    }
    /* (the value is quoted in os-release) */
    if (os_version[0] == '"') {
        memmove(os_version, os_version + 1, strlen(os_version));
        char *q = strchr(os_version, '"');
        if (q) {
            *q = '\0';
        }
    }
}

/* A logo: a blue tile with a "v". */
static void logo(int x, int y, int size) {
    if (ui.measuring) {
        return;
    }
    gfx_fill_rounded_rect(ui.s, gfx_rect(x, y, size, size), size * 0.24f, ACCENT);
    float s = size / 64.0f;
    gfx_draw_line(ui.s, x + 18 * s, y + 20 * s, x + 32 * s, y + 46 * s, 6 * s, GFX_RGB(255, 255, 255));
    gfx_draw_line(ui.s, x + 32 * s, y + 46 * s, x + 46 * s, y + 20 * s, 6 * s, GFX_RGB(255, 255, 255));
}

void page_general(void) {
    char buf[96];
    sysinfo(&sys);

    ui_card_begin();
    struct gfx_rect r = ui_row(150);
    logo(r.x + r.w / 2 - 32, r.y + 18, 64);
    float tw = ui_text_width(24, true, "viralOS");
    ui_text(24, true, r.x + (r.w - tw) / 2, r.y + 112, INK, "viralOS");
    snprintf(buf, sizeof(buf), "Version %s", os_version);
    tw = ui_text_width(FONT_SMALL, false, buf);
    ui_text(FONT_SMALL, false, r.x + (r.w - tw) / 2, r.y + 134, INK_SOFT, buf);
    ui_card_end();

    ui_card_begin();
    if (ui_field_row("Computer name", &hostname, "viralos", true)) {
        save_hostname();
    }
    ui_card_end();
    ui_note("Shown in the terminal and given to the network (DHCP). Press Enter to save it.");

    ui_heading("About");
    ui_card_begin();
    ui_info_row("Processor", cpu_name);
    snprintf(buf, sizeof(buf), "%u", sys.cpus);
    ui_info_row("Cores in use", buf);
    human_size(sys.memory_total, buf, sizeof(buf));
    ui_info_row("Memory", buf);
    snprintf(buf, sizeof(buf), "%d x %d", gui_screen_width(ui.g), gui_screen_height(ui.g));
    ui_info_row("Display", buf);
    uint64_t up = sys.uptime_ms / 1000;
    snprintf(buf, sizeof(buf), "%lu h %02lu min", (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60));
    ui_info_row("Up for", buf);
    ui_card_end();

    ui_heading("Software");
    ui_card_begin();
    snprintf(buf, sizeof(buf), "viralOS %s (x86_64)", os_version);
    ui_info_row("System", buf);
    ui_info_row("Kernel", "viralKernel " VIRALKERNEL_VERSION);
    ui_info_row("Desktop", "DDE 1.0 (Divine Desktop Environment)");
    snprintf(buf, sizeof(buf), "%u running", sys.processes);
    ui_info_row("Programs", buf);
    ui_card_end();
}

/* --- Displays ------------------------------------------------------------------------------------ */

void page_displays(void) {
    char buf[64];
    int w = gui_screen_width(ui.g), h = gui_screen_height(ui.g);

    ui_card_begin();
    struct gfx_rect r = ui_row(170);
    /* A little monitor, the screen's shape. */
    int mh = 100, mw = h ? mh * w / h : 160;
    mw = mw > 220 ? 220 : mw;
    int mx = r.x + (r.w - mw) / 2, my = r.y + 18;
    if (!ui.measuring) {
        gfx_fill_rounded_rect(ui.s, gfx_rect(mx - 6, my - 6, mw + 12, mh + 12), 10, GFX_RGB(0x2C, 0x2C, 0x30));
        gfx_fill_gradient(ui.s, gfx_rect(mx, my, mw, mh), GFX_RGB(0x3A, 0x7B, 0xF0), GFX_RGB(0x6E, 0xC6, 0xF5));
        gfx_fill_rect(ui.s, gfx_rect(mx + mw / 2 - 14, my + mh + 6, 28, 12), GFX_RGB(0x9A, 0x9A, 0xA2));
        gfx_fill_rounded_rect(ui.s, gfx_rect(mx + mw / 2 - 34, my + mh + 16, 68, 6), 3, GFX_RGB(0x9A, 0x9A, 0xA2));
    }
    float tw = ui_text_width(FONT_BODY, true, "Built-in Display");
    ui_text(FONT_BODY, true, r.x + (r.w - tw) / 2, r.y + 160, INK, "Built-in Display");
    ui_card_end();

    ui_card_begin();
    snprintf(buf, sizeof(buf), "%d x %d", w, h);
    ui_info_row("Resolution", buf);
    ui_info_row("Colours", "32-bit (millions of colours)");
    ui_info_row("Scaling", "100%");
    ui_info_row("Graphics", "Firmware framebuffer");
    ui_card_end();
    ui_note("The resolution is the one the firmware (UEFI or BIOS) chose when the computer started; "
            "changing it needs a graphics driver, which viralOS doesn't have yet.");
}

/* --- Date & Time --------------------------------------------------------------------------------- */

static const char *const weekdays[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const months[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                      "September", "October", "November", "December" };

void page_datetime(void) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    bool clock24 = config_get_int(DDE_CONFIG, "Clock24", 1) != 0;

    ui_card_begin();
    struct gfx_rect r = ui_row(130);
    char clock[32], date[64];
    if (clock24) {
        snprintf(clock, sizeof(clock), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        int h = tm.tm_hour % 12;
        snprintf(clock, sizeof(clock), "%d:%02d:%02d %s", h ? h : 12, tm.tm_min, tm.tm_sec, tm.tm_hour < 12 ? "AM" : "PM");
    }
    snprintf(date, sizeof(date), "%s, %d %s %d", weekdays[tm.tm_wday], tm.tm_mday, months[tm.tm_mon], tm.tm_year + 1900);
    float tw = ui_text_width(44, true, clock);
    ui_text(44, true, r.x + (r.w - tw) / 2, r.y + 72, INK, clock);
    tw = ui_text_width(FONT_BODY + 1, false, date);
    ui_text(FONT_BODY + 1, false, r.x + (r.w - tw) / 2, r.y + 104, INK_SOFT, date);
    ui_card_end();

    ui_card_begin();
    if (ui_toggle_row("24-hour time", "In the panel's clock and here", &clock24)) {
        config_set_int(DDE_CONFIG, "Clock24", clock24);
        gui_settings_changed(ui.g, -1, -1);
    }
    ui_info_row("Time zone", "The computer's clock (local time)");
    ui_card_end();
    ui_note("The time comes from the computer's real-time clock, which keeps local time (as Windows sets it). "
            "Setting the time and time zones from here will come later.");
}

/* --- Storage ------------------------------------------------------------------------------------- */

#define MAX_DRIVES 16

static struct os_drive_info drives_list[MAX_DRIVES];
static int drive_count;
static struct gfx_surface *icon_internal, *icon_external;

static struct gfx_surface *load_icon(const char *path, int size) {
    struct gfx_surface *img = image_load(path, NULL);
    if (!img) {
        return NULL;
    }
    struct gfx_surface *s = gfx_surface_scaled(img, size, size);
    gfx_surface_destroy(img);
    return s;
}

void page_storage_show(void) {
    if (!icon_internal) {
        icon_internal = load_icon(_PATH_DDE_RES "/Icons/FilesFolders/InternalStorage.png", 40);
        icon_external = load_icon(_PATH_DDE_RES "/Icons/FilesFolders/ExternalStorage.png", 40);
    }
    drive_count = 0;
    while (drive_count < MAX_DRIVES && drives(drive_count, &drives_list[drive_count]) == 0) {
        drive_count++;
    }
}

void page_storage(void) {
    if (drive_count == 0) {
        ui_card_begin();
        ui_info_row("No drives are mounted", "");
        ui_card_end();
    } else {
        ui_card_begin();
        for (int i = 0; i < drive_count; i++) {
            struct os_drive_info *dv = &drives_list[i];
            bool external = dv->flags & OS_DRIVE_EXTERNAL;
            struct gfx_rect r = ui_row(84);
            struct gfx_surface *icon = external ? icon_external : icon_internal;
            if (icon && !ui.measuring) {
                gfx_blit(ui.s, r.x + 16, r.y + 20, icon, NULL);
            }
            const char *name = dv->label[0] ? dv->label : dv->model[0] ? dv->model : dv->device;
            int right = external ? 110 : 16;
            ui_text_clipped(FONT_BODY, true, r.x + 70, r.y + 26, r.w - 70 - right - 120, INK, name);
            char used[24], total[24], line[96];
            human_size(dv->total - dv->free, used, sizeof(used));
            human_size(dv->total, total, sizeof(total));
            snprintf(line, sizeof(line), "%s used of %s", used, total);
            float lw = ui_text_width(FONT_SMALL, false, line);
            ui_text(FONT_SMALL, false, r.x + r.w - right - lw, r.y + 26, INK_SOFT, line);
            float frac = dv->total ? (float)(dv->total - dv->free) / dv->total : 0;
            ui_bar(gfx_rect(r.x + 70, r.y + 38, r.w - 70 - right, 8), frac,
                   frac > 0.9f ? DANGER : ACCENT);
            snprintf(line, sizeof(line), "%s  -  %s%s  -  %s", dv->filesystem, dv->device,
                     dv->flags & OS_DRIVE_READ_ONLY ? ", read-only" : "", dv->path);
            ui_text_clipped(FONT_SMALL, false, r.x + 70, r.y + 66, r.w - 70 - right, INK_SOFT, line);
            if (external) {
                if (ui_button(gfx_rect(r.x + r.w - 96, r.y + 28, 80, 28), "Eject", false, true)) {
                    int e = eject(dv->path);
                    if (e < 0) {
                        settings_message(GUI_ICON_ERROR, "Couldn't eject the drive", strerror(e));
                    } else {
                        settings_message(GUI_ICON_INFO, "Safe to remove", "The drive can be unplugged now.");
                    }
                    page_storage_show();
                }
            }
        }
        ui_card_end();
    }
    ui_card_begin();
    if (ui_button_row("Disks", "Partition, format, resize and repair drives", "Open Disks", false)) {
        settings_launch(_PATH_BIN "/divine-disks", NULL);
    }
    ui_card_end();
}
