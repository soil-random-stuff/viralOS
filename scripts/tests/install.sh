# The installer, end to end: boots the ISO with a blank 2 GB SATA disk, installs viralOS on it
# (account "alex", password "pw", administrator), then starts QEMU again from that disk alone and
# logs in. UEFI=1 does it all with OVMF (UEFI) instead of SeaBIOS.
# Screenshots: build/inst-*.png. Run from os-source/ in WSL:  bash scripts/tests/install.sh
set -e
mkdir -p build/live
rm -f build/live/inst.img
truncate -s 2G build/live/inst.img
FW=""
[ -n "$UEFI" ] && FW="-bios /usr/share/qemu/OVMF.fd"
QEMU_EXTRA="$FW" SATA=build/live/inst.img python3 scripts/drive.py wait:28 key:ret wait:8 dclick:57,145 wait:4 \
  key:ret wait:1 "type:yes" key:ret wait:2 click:670,196 wait:1 shot:inst-disk key:ret wait:1 \
  "type:Alex Smith" key:tab key:tab "type:pw" key:tab "type:pw" wait:1 key:ret wait:1 shot:inst-summary \
  key:ret wait:15 shot:inst-progress wait:60 shot:inst-done >/dev/null 2>&1
grep -iE "panic|killed" build/serial.log || true
# The new system partition, as the installer left it (unmounted): it must be clean.
bash scripts/tests/fsck-installed.sh build/live/inst.img
# The installed system, on its own.
QEMU_EXTRA="$FW" NO_CD=1 SATA=build/live/inst.img python3 scripts/drive.py wait:35 shot:inst-login \
  "type:pw" key:ret wait:10 shot:inst-desktop click:316,758 wait:4 "type:whoami -l" key:ret \
  "type:id" key:ret "type:cat /etc/hostname /etc/locale.conf" key:ret "type:ls /home /" key:ret wait:2 \
  shot:inst-terminal >/dev/null 2>&1
grep -iE "Firmware|Root filesystem|panic|killed" build/serial.log || true
