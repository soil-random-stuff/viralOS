#!/bin/bash
# Build the disk tools from their upstream sources (next to os-source/ in the osproject folder) as
# static musl programs for viralOS, into os-source/ports/bin (run from anywhere, in WSL):
#
#   dosfstools        mkfs.fat  fatlabel  fsck.fat
#   exfatprogs        mkfs.exfat  tune.exfat  exfatlabel  fsck.exfat
#   e2fsprogs         mke2fs  tune2fs  e2fsck  resize2fs
#   ntfsprogs-plus    mkntfs  ntfslabel  ntfsresize  ntfsck  ntfsinfo
#   parted + fatresize  fatresize
#   servers           ntfsd  (read-write NTFS for userfs, from ports/src; ext4 is in the kernel)
#   limine            limine  (bios-install, for the installer)
#
# They run unmodified through the kernel's Linux layer (src/proc/linux.c); scripts/mark_linux.py
# marks them so. Building happens in ~/ports-work (WSL's own disk: much faster than /mnt/c).
#   build_ports.sh [dosfstools|exfatprogs|e2fsprogs|ntfsprogs]...   (default: all)
set -e
OS=$(cd "$(dirname "$0")/.." && pwd)
SRC=${PORTS_SRC:-$(cd "$OS/.." && pwd)}   # the upstream sources (scripts/fetch_ports.sh)
OUT=$OS/ports/bin
WORK=$HOME/ports-work
CC="$OS/scripts/musl-cc"
JOBS=$(nproc)
mkdir -p "$OUT" "$WORK"

fresh_copy() {   # fresh_copy <source folder name>: a clean copy in $WORK
    rm -rf "$WORK/$1"
    cp -a "$SRC/$1" "$WORK/$1"   # (-a: keeps the times, or autotools wants to regenerate)
}

finish() {   # finish <binary>...: mark as Linux programs, strip, copy to ports/bin
    for b in "$@"; do
        strip "$b"
        python3 "$OS/scripts/mark_linux.py" "$b"
        cp "$b" "$OUT/"
        echo "  $(basename "$b"): $(stat -c %s "$b") bytes"
    done
}

build_dosfstools() {
    echo "== dosfstools"
    fresh_copy dosfstools-master
    local S=$WORK/dosfstools-master/src
    sed -e 's/@PACKAGE_VERSION@/4.2+git/' -e 's/@RELEASE_DATE@/2021-01-31/' -e 's/@configure_input@//' \
        "$S/version.h.in" > "$S/version.h"
    local CF="-O2 -w -I$S -I$S/blkdev -DHAVE_ENDIAN_H -DHAVE_VASPRINTF -DHAVE_ERR_H -DHAVE_LINUX_HDREG_H \
-DHAVE_LINUX_FD_H -DHAVE_LINUX_LOOP_H -DHAVE_LINUX_VERSION_H -DHAVE_SYS_SYSMACROS_H -DHAVE_SYS_QUEUE_H \
-DHAVE_DECL_GETMNTENT=1 -DHAVE_DECL_GETMNTINFO=0"
    cd "$S"
    $CC $CF -o mkfs.fat mkfs.fat.c common.c charconv.c device_info.c blkdev/blkdev.c blkdev/linux_version.c
    $CC $CF -o fatlabel fatlabel.c boot.c common.c fat.c io.c charconv.c
    $CC $CF -o fsck.fat fsck.fat.c check.c file.c lfn.c boot.c common.c fat.c io.c charconv.c
    finish mkfs.fat fatlabel fsck.fat
}

build_exfatprogs() {
    echo "== exfatprogs"
    fresh_copy exfatprogs-master
    local S=$WORK/exfatprogs-master
    printf '#define PACKAGE "exfatprogs"\n#define HAVE_FTS_H 1\n' > "$S/config.h"
    local CF="-O2 -w -include $S/config.h -I$S/include -fno-common"
    local LIB="$S/lib/libexfat.c $S/lib/exfat_fs.c $S/lib/exfat_dir.c $S/lib/utils.c"
    # mkfs.exfat looks for an old filesystem with libblkid: e2fsprogs' one (build e2fsprogs first).
    local E2=$WORK/e2fsprogs-build/lib
    [ -f "$E2/libblkid.a" ] || build_e2fsprogs
    cd "$S"
    $CC $CF -DHAVE_EXT2_BLKID=1 -I"$E2/blkid" -I"$E2" -o mkfs.exfat mkfs/mkfs.c mkfs/upcase.c mkfs/crc.c $LIB \
        "$E2/libblkid.a" "$E2/libuuid.a"
    $CC $CF -o tune.exfat tune/tune.c $LIB
    $CC $CF -o exfatlabel label/label.c $LIB
    $CC $CF -o fsck.exfat fsck/fsck.c fsck/repair.c $LIB
    finish mkfs.exfat tune.exfat exfatlabel fsck.exfat
}

build_e2fsprogs() {
    echo "== e2fsprogs"
    fresh_copy e2fsprogs-master
    local S=$WORK/e2fsprogs-master B=$WORK/e2fsprogs-build
    rm -rf "$B" && mkdir -p "$B" && cd "$B"
    CC="$CC" LDFLAGS="-static" "$S/configure" --host=x86_64-linux-musl --build=x86_64-linux-gnu \
        --disable-nls --disable-fuse2fs --disable-defrag --disable-imager --disable-debugfs \
        --disable-uuidd --disable-e2initrd-helper --disable-elf-shlibs --disable-bsd-shlibs \
        --disable-backtrace --disable-tdb --without-libarchive --without-pthread \
        --enable-libuuid --enable-libblkid --disable-rpath > configure.log 2>&1 || { tail -30 configure.log; exit 1; }
    make -j"$JOBS" libs > make-libs.log 2>&1 || { tail -40 make-libs.log; exit 1; }
    make -j"$JOBS" -C misc mke2fs tune2fs > make-misc.log 2>&1 || { tail -40 make-misc.log; exit 1; }
    make -j"$JOBS" -C resize resize2fs > make-resize.log 2>&1 || { tail -40 make-resize.log; exit 1; }
    make -j"$JOBS" -C e2fsck e2fsck > make-e2fsck.log 2>&1 || { tail -40 make-e2fsck.log; exit 1; }
    finish misc/mke2fs misc/tune2fs resize/resize2fs e2fsck/e2fsck
}

build_ntfsprogs() {
    echo "== ntfsprogs-plus"
    fresh_copy ntfsprogs-plus-main
    local S=$WORK/ntfsprogs-plus-main
    cat > "$S/config.h" <<'EOF'
/* Written by hand for viralOS (the upstream build needs autotools). */
#define PACKAGE "ntfsprogs-plus"
#define PACKAGE_NAME "ntfsprogs-plus"
#define PACKAGE_VERSION "1.0.0"
#define VERSION "1.0.0"
#define HAVE_CTYPE_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_GETOPT_H 1
#define HAVE_LIBGEN_H 1
#define HAVE_LIMITS_H 1
#define HAVE_LOCALE_H 1
#define HAVE_MNTENT_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_SYSLOG_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_MOUNT_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_SYSMACROS_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_TIME_H 1
#define HAVE_UNISTD_H 1
#define HAVE_WCHAR_H 1
#define HAVE_BYTESWAP_H 1
#define HAVE_ENDIAN_H 1
#define HAVE_FEATURES_H 1
#define HAVE_MALLOC_H 1
#define HAVE_REGEX_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1
#define HAVE_SYS_STATVFS_H 1
#define HAVE_SYS_VFS_H 1
#define HAVE_SYS_XATTR_H 1
#define HAVE_UTIME_H 1
#define HAVE_LINUX_FD_H 1
#define HAVE_LINUX_FS_H 1
#define HAVE_LINUX_HDREG_H 1
#define HAVE_LINUX_MAJOR_H 1
#define HAVE_MBSINIT 1
#define HAVE_STRSEP 1
#define HAVE_FFS 1
#define HAVE_REALPATH 1
#define HAVE_HASMNTOPT 1
#define HAVE_DAEMON 1
#define IGNORE_MTAB 1
EOF
    local CF="-O2 -w -DHAVE_CONFIG_H -include $S/config.h -include sys/sysmacros.h -I$S -I$S/include -I$S/include/ntfs -I$S/libntfs -I$S/src"
    cd "$S/libntfs"
    rm -f *.o
    for f in acls attrib attrlist bitmap bootsect cache collate compat compress debug device dir ea efs fsck \
             index inode lcnalloc lib_utils logfile logging mft misc mst object_id problem realpath reparse \
             runlist security unistr volume xattrs unix_io; do
        $CC $CF -c "$f.c" -o "$f.o"
    done
    ar rcs libntfs.a *.o
    cd "$S/src"
    $CC $CF -o mkntfs attrdef.c sd.c mkntfs.c utils.c ../libntfs/libntfs.a
    $CC $CF -o ntfslabel ntfslabel.c utils.c ../libntfs/libntfs.a
    $CC $CF -o ntfsresize ntfsresize.c utils.c ../libntfs/libntfs.a
    $CC $CF -o ntfsck ntfsck.c utils.c ../libntfs/libntfs.a
    $CC $CF -o ntfsinfo ntfsinfo.c utils.c ../libntfs/libntfs.a
    finish mkntfs ntfslabel ntfsresize ntfsck ntfsinfo
}

# fatresize: resizes FAT16/32 (and the partition) with GNU parted's libparted + libparted-fs-resize,
# built static here (no device-mapper, readline or translations; libuuid from e2fsprogs).
build_fatresize() {
    echo "== parted + fatresize"
    local E2B=$WORK/e2fsprogs-build E2S=$WORK/e2fsprogs-master
    [ -f "$E2B/lib/libuuid.a" ] || build_e2fsprogs
    fresh_copy parted-3.6
    local S=$WORK/parted-3.6
    # libuuid where configure looks for it.
    local U=$WORK/uuid-root
    rm -rf "$U" && mkdir -p "$U/include/uuid" "$U/lib"
    cp "$E2B/lib/uuid/uuid.h" "$E2B/lib/uuid/uuid_types.h" "$U/include/uuid/"
    cp "$E2B/lib/libuuid.a" "$U/lib/"
    cd "$S"
    CC="$CC" CPPFLAGS="-I$U/include" LDFLAGS="-static -L$U/lib" ./configure --host=x86_64-linux-musl \
        --build=x86_64-linux-gnu --disable-device-mapper --without-readline --disable-nls --disable-shared \
        --enable-static > configure.log 2>&1 || { tail -30 configure.log; exit 1; }
    make -j"$JOBS" -C lib > make-lib.log 2>&1 || { tail -40 make-lib.log; exit 1; }
    make -j"$JOBS" -C libparted > make-libparted.log 2>&1 || { tail -40 make-libparted.log; exit 1; }
    fresh_copy fatresize-master
    local F=$WORK/fatresize-master
    printf '#define PACKAGE "fatresize"\n#define PACKAGE_NAME "fatresize"\n#define PACKAGE_STRING "fatresize 1.1.0"\n#define PACKAGE_BUGREPORT "https://github.com/ya-mouse/fatresize"\n#define BUILD_DATE "viralOS"\n' \
        > "$F/config.h"
    # Both libraries have their own FAT/HFS helpers under the same names (as shared libraries they
    # don't meet). Linked statically, the resize library's copies get names of their own.
    local P=$S/libparted/.libs/libparted.a R=$S/libparted/fs/.libs/libparted-fs-resize.a
    comm -12 <(nm -g --defined-only "$P" | awk 'NF==3 {print $3}' | sort -u) \
             <(nm -g --defined-only "$R" | awk 'NF==3 {print $3}' | sort -u) | awk '{print $1, "resize_" $1}' > "$F/dups"
    objcopy --redefine-syms="$F/dups" "$R" "$F/libparted-fs-resize-own.a"
    cd "$F"
    $CC -O2 -w -I"$S/include" -I"$F" -o fatresize fatresize.c "$F/libparted-fs-resize-own.a" "$P" \
        "$S/lib/.libs/libgnulib.a" "$U/lib/libuuid.a"
    finish fatresize
}

# The read-write filesystem servers for userfs (os-source/ports/src), on the libraries built above.
build_servers() {
    echo "== servers"
    local P=$OS/ports/src NS=$WORK/ntfsprogs-plus-main
    [ -f "$NS/libntfs/libntfs.a" ] || build_ntfsprogs
    mkdir -p "$WORK/servers" && cd "$WORK/servers"
    if [ -f "$P/ntfsd.c" ]; then
        $CC -O2 -Wall -Wno-unused-parameter -DHAVE_CONFIG_H -include "$NS/config.h" -include sys/sysmacros.h \
            -I"$OS/abi" -I"$NS" -I"$NS/include" -I"$NS/include/ntfs" -I"$NS/libntfs" -o ntfsd \
            "$P/ntfsd.c" "$P/ufs_server.c" "$NS/libntfs/libntfs.a"
        finish ntfsd
    fi
}

# limine: the bootloader's own installer ("limine bios-install /dev/sata0"), which the viralOS installer
# runs to make a disk start on BIOS PCs. From the Limine binary release the Makefile downloads.
build_limine() {
    echo "== limine"
    [ -f "$OS/limine/limine.c" ] || git clone --depth=1 --branch=v11.x-binary \
        https://github.com/limine-bootloader/limine.git "$OS/limine"
    mkdir -p "$WORK/limine" && cd "$WORK/limine"
    $CC -O2 -w -I"$OS/limine" -o limine "$OS/limine/limine.c"
    finish limine
}

targets=("$@")
[ ${#targets[@]} -eq 0 ] && targets=(dosfstools exfatprogs e2fsprogs ntfsprogs fatresize servers limine)
for t in "${targets[@]}"; do
    "build_$t"
done
echo "done: $(ls "$OUT" | tr '\n' ' ')"
