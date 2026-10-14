# Shutting down carefully, with a FAT USB stick plugged in (a fresh copy of build/disks/fat32.img).
#   1. An unsaved Text Editor and a ping writing to the stick: "An app didn't close" (build/sd-apps.png),
#      Shut down anyway; then "A drive couldn't be unmounted" (sd-drive.png), Cancel: back to the desktop.
#   2. A file written to the stick, then Shut down: the stick is unmounted and the file is on the image.
# Run from os-source/ in WSL:  bash scripts/tests/shutdown.sh
mkdir -p build/live
cp build/disks/fat32.img build/live/t-fat.img
DISKS=build/live/t-fat.img python3 scripts/drive.py wait:24 key:ret wait:8 click:316,758 wait:4 \
  "type:ping 10.0.2.2 > /media/TESTFAT32/ping.txt &" key:ret wait:2 click:46,758 wait:2 click:171,595 wait:4 \
  "type:unsaved notes" wait:1 click:46,758 wait:2 click:380,692 wait:2 click:279,581 wait:6 shot:sd-apps \
  key:ret wait:3 shot:sd-drive key:esc wait:2 >/dev/null 2>&1
cp build/disks/fat32.img build/live/t-fat.img
DISKS=build/live/t-fat.img python3 scripts/drive.py wait:24 key:ret wait:8 click:316,758 wait:4 \
  "type:echo saved before shutdown > /media/TESTFAT32/hi.txt" key:ret wait:1 \
  click:46,758 wait:2 click:380,692 wait:2 click:279,581 wait:10 >/dev/null 2>&1
grep -iE "unmounted|Powering off|panic" build/serial.log
grep -c "saved before shutdown" build/live/t-fat.img >/dev/null && echo "hi.txt is on the stick"
