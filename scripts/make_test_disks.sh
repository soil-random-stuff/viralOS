#!/bin/bash
# Disk images for testing the storage drivers, in build/disks/ (run from os-source, in WSL):
#
#   fat32.img   MBR, partition 1 at 1 MiB: FAT32 "TESTFAT32"
#   fat16.img   no partition table ("superfloppy"): FAT16 "FAT16TEST"
#   exfat.img   GPT, partition 1: exFAT "EXFATTEST" (empty)
#   ext4.img    GPT, partition 1: ext4 "LINUXDISK"
#   ntfs.img    MBR, partition 1: NTFS "NTFSTEST"
#   multi.img   MBR: FAT32 primary + extended with two logical (exFAT, ext4)
#
# Each (but exFAT) holds the same files (build/disks/content), so the OS can be checked against them.
# Tools: ~/fstools (dosfstools, mtools, exfatprogs, ntfs-3g unpacked there with `apt download` +
# `dpkg -x`, no root needed), e2fsprogs and sfdisk/sgdisk from the system. NTFS is filled through
# libntfs-3g (scripts/ntfs_fill.py): mounting it with FUSE would need root.
set -e
T=~/fstools/root
export PATH=$T/usr/sbin:$T/usr/bin:$PATH
export LD_LIBRARY_PATH=$T/usr/lib/x86_64-linux-gnu
export MTOOLS_SKIP_CHECK=1
D=build/disks
M=/tmp/viralos-mnt
fusermount -u $M 2>/dev/null || true
rm -rf $D
mkdir -p $D/content $M

# --- the files --------------------------------------------------------------------------------------
C=$D/content
printf 'Hello from a USB stick!\n' > $C/hello.txt
printf 'This file has a long name with spaces.\n' > "$C/A long file name with spaces.txt"
printf 'Unicode: caf\xc3\xa9, \xc3\xb1and\xc3\xba, \xe6\x97\xa5\xe6\x9c\xac\n' > "$C/Caf$(printf '\xc3\xa9') $(printf '\xc3\xb1')and$(printf '\xc3\xba').txt"
mkdir -p "$C/Folder/Sub folder"
printf 'deep\n' > "$C/Folder/Sub folder/deep.txt"
printf 'in folder\n' > "$C/Folder/note.txt"
mkdir -p $C/Compressed
python3 -c "
import random
r = random.Random(1234)
open('$C/big.bin', 'wb').write(bytes(r.getrandbits(8) for _ in range(3 * 1024 * 1024 + 123)))
# For NTFS compression (ntfs_fill.py makes the Compressed folder compressed): all-zero units (stored
# sparse), repetitive text (LZNT1), random bytes (stored raw because they don't compress).
text = b''.join(b'line %d: the quick brown fox jumps over the lazy dog\n' % i for i in range(6000))
open('$C/Compressed/compressed.bin', 'wb').write(bytes(70000) + text + bytes(r.getrandbits(8) for _ in range(150000)) + text[:5000])
"
mkdir -p "$C/Many"
for i in $(seq 1 300); do printf 'file %d\n' $i > "$C/Many/file_$i.txt"; done
README="README.TXT"
printf 'an 8.3 name\n' > $C/$README

copy_tree() {   # copy_tree <dest dir>
    cp -r $C/. "$1"/
}

# --- FAT32 on an MBR partition ------------------------------------------------------------------------
truncate -s 128M $D/fat32.img
echo 'start=2048, type=c' | sfdisk -q $D/fat32.img
mkfs.fat -F 32 -n TESTFAT32 --offset 2048 $D/fat32.img > /dev/null
mcopy -s -i $D/fat32.img@@1M $C/* ::/

# --- FAT16 superfloppy ----------------------------------------------------------------------------------
truncate -s 32M $D/fat16.img
mkfs.fat -F 16 -n FAT16TEST $D/fat16.img > /dev/null
mcopy -s -i $D/fat16.img $C/* ::/

# --- exFAT on GPT -----------------------------------------------------------------------------------------
part_gpt() {   # part_gpt <image> <size> <type code>: one partition at 1 MiB; leaves the partition in $1.part
    truncate -s $2 $1
    sgdisk -o -n 1:2048:0 -t 1:$3 $1 > /dev/null
    local first=$(sgdisk -i 1 $1 | awk '/First sector/ {print $3}') last=$(sgdisk -i 1 $1 | awk '/Last sector/ {print $3}')
    truncate -s $(( (last - first + 1) * 512 )) $1.part
}
put_part() {   # put_part <image> <start sector>
    dd if=$1.part of=$1 bs=512 seek=$2 conv=notrunc status=none
    rm $1.part
}
# (Empty: exfat-fuse wants allow_other, which needs root. The OS writes the files and fsck.exfat checks them.)
part_gpt $D/exfat.img 96M 0700
mkfs.exfat -q -L EXFATTEST $D/exfat.img.part > /dev/null
put_part $D/exfat.img 2048

# --- ext4 on GPT ---------------------------------------------------------------------------------------------
part_gpt $D/ext4.img 64M 8300
mkfs.ext4 -q -F -L LINUXDISK -d $C $D/ext4.img.part
put_part $D/ext4.img 2048

# --- NTFS on MBR -----------------------------------------------------------------------------------------------
truncate -s 96M $D/ntfs.img
echo 'start=2048, type=7' | sfdisk -q $D/ntfs.img
truncate -s $(( 96 * 1024 * 1024 - 2048 * 512 )) $D/ntfs.img.part
mkntfs -q -F -Q -L NTFSTEST -p 2048 $D/ntfs.img.part > /dev/null
python3 scripts/ntfs_fill.py $D/ntfs.img.part $C
put_part $D/ntfs.img 2048

# --- several partitions: FAT32 primary, extended with exFAT and ext4 logical ones -------------------------------
truncate -s 200M $D/multi.img
sfdisk -q $D/multi.img > /dev/null <<'EOF'
label: dos
start=2048, size=80M, type=c
start=165888, type=5
start=167936, size=50M, type=7
start=272384, size=40M, type=83
EOF
mkfs.fat -F 32 -n MULTIFAT --offset 2048 $D/multi.img $(( 80 * 1024 )) > /dev/null
mcopy -i $D/multi.img@@1M $C/hello.txt ::/
truncate -s 50M $D/multi.img.part
mkfs.exfat -q -L MULTIEXFAT $D/multi.img.part > /dev/null
put_part $D/multi.img 167936
truncate -s 40M $D/multi.img.part
mkfs.ext4 -q -F -L MULTIEXT4 -d $D/content/Folder $D/multi.img.part
put_part $D/multi.img 272384

ls -la $D
