# Allow writing on the ext4 disk in Disks, change files through it, eject; check with e2fsck/debugfs.
. scripts/tests/rw-common.sh
rw_session 2 /media/LINUXDISK
dd if=build/sata-ext4.img of=/tmp/e.img bs=512 skip=2048 status=none
e2fsck -fn /tmp/e.img 2>&1 | tail -1; dumpe2fs -h /tmp/e.img 2>/dev/null | grep -i "state"
debugfs -R "ls -l /newdir" /tmp/e.img 2>/dev/null; debugfs -R "cat /newdir/Hello.txt" /tmp/e.img 2>/dev/null
grep -iE "ext4d|panic|WARN" build/serial.log | tail -4
