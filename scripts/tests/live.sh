# A QEMU window with sound and four writable drives to try things on (fresh copies each time).
cd "$(dirname "$0")/../.."
mkdir -p build/live
cp build/disks/fat32.img build/live/usb-fat32.img
cp build/disks/ntfs.img build/live/nvme-ntfs.img
cp build/disks/ext4.img build/live/sata-ext4.img
rm -f build/live/usb-blank.img; truncate -s 1G build/live/usb-blank.img
exec qemu-system-x86_64 -M q35 -cpu max -smp 4 -m 1G -rtc base=localtime -cdrom viralos.iso -boot order=d \
  -audiodev pa,id=snd0 -device ich9-intel-hda -device hda-output,audiodev=snd0 \
  -display gtk -serial file:build/live/serial.log \
  -device qemu-xhci,id=xhci \
  -drive if=none,id=u0,file=build/live/usb-fat32.img,format=raw -device usb-storage,bus=xhci.0,drive=u0 \
  -drive if=none,id=u1,file=build/live/usb-blank.img,format=raw -device usb-storage,bus=xhci.0,drive=u1 \
  -drive if=none,id=nv0,file=build/live/nvme-ntfs.img,format=raw -device nvme,serial=NVME0001,drive=nv0 \
  -drive if=none,id=sd0,file=build/live/sata-ext4.img,format=raw -device "ide-hd,drive=sd0,bus=ide.3,model=KINGSTON SA400S37240G"
