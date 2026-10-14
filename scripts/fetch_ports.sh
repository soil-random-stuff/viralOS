#!/bin/bash
# Download what scripts/build_ports.sh needs (the GitHub Actions build runs this; on your own machine
# you only need it once):
#
#   the upstream sources, at the versions viralOS is built with, into PORTS_SRC (default: the folder
#   next to os-source), named as build_ports.sh expects (dosfstools-master, e2fsprogs-master, ...)
#   musl and Linux's headers: Ubuntu's musl, musl-dev and linux-libc-dev packages unpacked into
#   ~/fstools (apt-get download + dpkg -x: no root needed)
#
#   scripts/fetch_ports.sh          everything that's missing
set -e
OS=$(cd "$(dirname "$0")/.." && pwd)
SRC=${PORTS_SRC:-$(cd "$OS/.." && pwd)}
mkdir -p "$SRC"

# name of the folder build_ports.sh wants | tarball URL | the folder the tarball unpacks to
SOURCES=(
    "dosfstools-master|https://github.com/dosfstools/dosfstools/archive/289a48b9cb5b3c589391d28aa2515c325c932c7a.tar.gz|dosfstools-289a48b9cb5b3c589391d28aa2515c325c932c7a"
    "exfatprogs-master|https://github.com/exfatprogs/exfatprogs/archive/8b427483164888f0a5af5e4ba92da4981f1094e2.tar.gz|exfatprogs-8b427483164888f0a5af5e4ba92da4981f1094e2"
    "e2fsprogs-master|https://github.com/tytso/e2fsprogs/archive/8fd79523d051d5ea881237f27eb1c664486e0078.tar.gz|e2fsprogs-8fd79523d051d5ea881237f27eb1c664486e0078"
    "ntfsprogs-plus-main|https://github.com/ntfsprogs-plus/ntfsprogs-plus/archive/3fe890434082973aa98fd441d65ae84e5d6e5fc8.tar.gz|ntfsprogs-plus-3fe890434082973aa98fd441d65ae84e5d6e5fc8"
    "fatresize-master|https://github.com/ya-mouse/fatresize/archive/75cbc388dc7fc6add789812a3f7ddaead2d44379.tar.gz|fatresize-75cbc388dc7fc6add789812a3f7ddaead2d44379"
    "parted-3.6|https://ftp.gnu.org/gnu/parted/parted-3.6.tar.xz|parted-3.6"
)

for entry in "${SOURCES[@]}"; do
    IFS='|' read -r name url unpacked <<< "$entry"
    if [ -d "$SRC/$name" ]; then
        continue
    fi
    echo "== $name"
    tmp=$(mktemp -d)
    curl -fsSL "$url" -o "$tmp/src.tar"
    tar -xf "$tmp/src.tar" -C "$tmp"
    mv "$tmp/$unpacked" "$SRC/$name"
    rm -rf "$tmp"
done

# musl and the kernel headers (for scripts/musl-cc).
F=$HOME/fstools
if [ ! -f "$F/root/usr/lib/x86_64-linux-musl/musl-gcc.specs" ] || [ ! -f "$F/kh/usr/include/linux/fs.h" ]; then
    echo "== musl, musl-dev, linux-libc-dev"
    mkdir -p "$F/debs" && cd "$F/debs"
    apt-get download musl musl-dev linux-libc-dev
    for deb in musl_*.deb musl-dev_*.deb; do
        dpkg -x "$deb" "$F/root"
    done
    dpkg -x linux-libc-dev_*.deb "$F/kh"
fi
echo "fetch_ports: sources in $SRC, musl in $F"
