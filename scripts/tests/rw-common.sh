# Shared by the read-write tests: boots with an NTFS NVMe disk and an ext4 SATA disk (fresh copies of
# build/disks, made by scripts/make_test_disks.sh), opens Disks and allows writing on one volume.
# Run from os-source/ in WSL.
[ -f build/disks/ntfs.img ] || bash scripts/make_test_disks.sh >/dev/null 2>&1
cp build/disks/ntfs.img build/nv-ntfs.img; cp build/disks/ext4.img build/sata-ext4.img
T=~/fstools/root
export PATH=$T/usr/sbin:$T/usr/bin:$T/bin:$PATH LD_LIBRARY_PATH=$T/usr/lib/x86_64-linux-gnu:$T/lib/x86_64-linux-gnu
# rw_session <disk 1|2> <mount point>: allow writing, then change files through it and eject.
rw_session() {
    local y=124 cy=367 by=423 m=$2
    [ "$1" = 2 ] && y=182 cy=380 by=436
    NVME=build/nv-ntfs.img SATA=build/sata-ext4.img python3 scripts/drive.py wait:24 key:ret wait:8 click:316,758 wait:4 \
      "type:divine-disks &" key:ret wait:6 \
      click:250,$y wait:2 click:700,330 wait:2 click:466,402 wait:2 click:540,530 wait:3 \
      click:385,439 click:761,484 wait:3 click:461,$cy click:737,$by wait:10 shot:rw-1 click:316,758 wait:2 \
      "type:mkdir $m/newdir" key:ret wait:2 \
      "type:echo hello from viralOS > $m/newdir/hello.txt" key:ret wait:2 \
      "type:cp /etc/motd $m/motd.txt" key:ret wait:2 \
      "type:mv $m/motd.txt $m/newdir/motd2.txt" key:ret wait:2 \
      "type:mv $m/newdir/hello.txt $m/newdir/Hello.txt" key:ret wait:2 \
      "type:ls $m/newdir" key:ret wait:2 shot:rw-2 \
      "type:eject $m" key:ret wait:5 >/dev/null 2>&1
}
