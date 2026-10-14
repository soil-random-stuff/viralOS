# Allow writing on the NTFS disk in Disks, change files through it, eject; check with ntfsck/ntfsls.
. scripts/tests/rw-common.sh
rw_session 1 /media/NTFSTEST
dd if=build/nv-ntfs.img of=/tmp/n.img bs=512 skip=2048 status=none
$HOME/ports-work/ntfsprogs-plus-main/src/ntfsck -n /tmp/n.img 2>&1 | tail -1
ntfsls -l -p /newdir /tmp/n.img 2>&1; ntfscat /tmp/n.img /newdir/Hello.txt 2>&1
grep -iE "ntfsd|panic|WARN" build/serial.log | tail -4
