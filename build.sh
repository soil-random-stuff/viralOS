#!/bin/bash
# Build everything: the ported disk tools and filesystem servers (scripts/build_ports.sh), then the
# kernel, the shared libraries, every program and app, the root filesystem and the bootable ISO
# (make). Nothing is downloaded: if something is missing, it says what and stops.
#
#   ./build.sh              everything
#   ./build.sh --no-ports   reuse ports/bin as it is (the ports take a few minutes to build)
#   ./build.sh --clean      start from scratch (build/ and the ports' work folder)
#   ./build.sh -j N         parallel jobs (default: all CPUs)
#
# Run it in Linux (WSL): from Windows,  wsl --cd <this folder> -- ./build.sh
set -e
cd "$(dirname "$0")"
OS=$(pwd)
SRC=${PORTS_SRC:-$(cd .. && pwd)}   # the ported tools' upstream sources (scripts/fetch_ports.sh)

ports=1
clean=0
jobs=$(nproc)
while [ $# -gt 0 ]; do
    case "$1" in
        --no-ports) ports=0 ;;
        --clean)    clean=1 ;;
        -j)         shift; jobs="$1" ;;
        -j*)        jobs="${1#-j}" ;;
        -h|--help)  sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)          echo "build.sh: unknown option '$1' (try --help)"; exit 2 ;;
    esac
    shift
done

missing=()
need_tool() {
    command -v "$1" >/dev/null 2>&1 || missing+=("the program '$1' ($2)")
}
need_path() {
    [ -e "$1" ] || missing+=("$1 ($2)")
}

# --- what has to be here already ----------------------------------------------------------------
need_tool gcc     "the compiler"
need_tool ld      "binutils"
need_tool ar      "binutils"
need_tool objcopy "binutils"
need_tool nm      "binutils"
need_tool strip   "binutils"
need_tool make    "make"
need_tool python3 "python3: makes the root filesystem"
need_tool xorriso "xorriso: makes the ISO"
need_path "$OS/limine/limine-bios.sys" "the Limine bootloader binaries (git clone --branch=v11.x-binary of limine)"
need_path "$OS/limine-protocol/include/limine.h" "the Limine protocol header"
if [ $ports = 1 ]; then
    need_path "$HOME/fstools/root/usr/lib/x86_64-linux-musl/musl-gcc.specs" \
        "musl: scripts/fetch_ports.sh sets it up"
    need_path "$HOME/fstools/kh/usr/include/linux/fs.h" \
        "Linux headers: scripts/fetch_ports.sh sets them up"
    for d in dosfstools-master exfatprogs-master e2fsprogs-master ntfsprogs-plus-main parted-3.6 fatresize-master; do
        need_path "$SRC/$d" "its source: scripts/fetch_ports.sh downloads it"
    done
else
    [ -n "$(ls ports/bin 2>/dev/null)" ] || missing+=("ports/bin (built ports: run without --no-ports once)")
fi
list=()
for m in "${missing[@]}"; do [ -n "$m" ] && list+=("$m"); done
if [ ${#list[@]} -gt 0 ]; then
    echo "Can't build, these are missing:"
    for m in "${list[@]}"; do echo "  - $m"; done
    exit 1
fi

step() {
    echo
    echo "=== $* ==="
}
started=$(date +%s)

if [ $clean = 1 ]; then
    step "Cleaning"
    rm -rf build "$HOME/ports-work"
fi

if [ $ports = 1 ]; then
    step "Ported tools and filesystem servers (scripts/build_ports.sh)"
    bash scripts/build_ports.sh
fi

step "Kernel, libraries, programs, root filesystem and ISO (make)"
log=build/make.log
mkdir -p build
if ! make -j"$jobs" all > "$log" 2>&1; then
    grep -iE "error|undefined" "$log" | head -30
    echo
    echo "make failed: the whole output is in $log"
    exit 1
fi
warnings=$(grep -ciE "warning" "$log" || true)

step "Done in $(( $(date +%s) - started )) s"
printf '  %-28s %s\n' "kernel" "build/kernel.elf ($(stat -c %s build/kernel.elf) bytes)"
printf '  %-28s %s\n' "programs (/usr/bin)" "$(ls build/user/bin | wc -l) built + $(ls ports/bin | wc -l) ported"
printf '  %-28s %s\n' "libraries (/usr/lib)" "$(ls build/user/Library | tr '\n' ' ')"
printf '  %-28s %s\n' "bootable ISO" "viralos.iso ($(( $(stat -c %s viralos.iso) / 1048576 )) MiB)"
[ "$warnings" -gt 0 ] && echo "  ($warnings compiler warnings: see $log)"
echo "  Run it with: make run"
