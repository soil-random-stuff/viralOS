#!/usr/bin/env python3
"""
Build the root filesystem image (initrd.tar) for viralOS.

    mkinitrd.py OUTPUT.tar SOURCE=DEST [SOURCE=DEST ...] [--sysroot DIR]

Each SOURCE (a file, or a directory copied recursively) is placed at DEST
inside the image. The standard system folders always exist, even if
nothing goes into them. With --sysroot, the assembled tree is also written
out as a normal directory, so you can see exactly what the OS will get.

Ownership and permissions don't come from the build machine (files under
/mnt/c all look like mode 777 to WSL); they follow these rules instead:
  directories                           0755 (/tmp: 1777)
  files in /usr/bin and /usr/lib        0755 (programs and libraries)
  /etc/shadow                           0600
  everything else                       0644
Everything belongs to root, but the home folders: to their user (uid and gid
1000 for /home/user, from /etc/passwd).
"""

import os
import shutil
import sys
import tarfile
import time

# The system folders. This list is the one place the layout is defined (see
# abi/paths.h for what each is).
SYSTEM_DIRS = [
    "dev",                      # disks and partitions (the kernel's devfs goes here)
    "etc",                      # system settings
    "etc/dde",
    "home",                     # the users' files
    "media",                    # mount points for USB sticks and other drives
    "root",                     # root's home
    "run",                      # what programs keep while running (sudo's timestamps)
    "tmp",                      # scratch space
    "usr",
    "usr/bin",                  # programs
    "usr/lib",                  # shared libraries (lib*.so)
    "usr/share",
    "usr/share/applications",   # app launchers (*.desktop)
    "usr/share/dde-res",        # the desktop's resources: fonts, wallpapers, icons, sounds, cursors
    "usr/share/doc",
    "var",
    "var/lib",
]

EXECUTABLE_DIRS = ("usr/bin/", "usr/lib/")
PRIVATE_FILES = ("etc/shadow",)
STICKY_DIRS = ("tmp",)
HOME_OWNERS = {"home/user": 1000}


# The OS keeps local time (its clock comes from the PC's real-time clock,
# which holds local time), so timestamps go into the image as local time too.
LOCAL_OFFSET = time.localtime().tm_gmtoff
BUILD_TIME = int(time.time()) + LOCAL_OFFSET


def mode_for(path, is_dir):
    if is_dir:
        return 0o1777 if path in STICKY_DIRS else 0o755
    if path in PRIVATE_FILES:
        return 0o600
    return 0o755 if path.startswith(EXECUTABLE_DIRS) else 0o644


def owner_for(path):
    for home, uid in HOME_OWNERS.items():
        if path == home or path.startswith(home + "/"):
            return uid
    return 0


def collect(mappings):
    """Return {image path: source path or None (directory)}."""
    entries = {d: None for d in SYSTEM_DIRS}

    def add_parents(path):
        parts = path.split("/")[:-1]
        for i in range(1, len(parts) + 1):
            entries.setdefault("/".join(parts[:i]), None)

    for mapping in mappings:
        source, sep, dest = mapping.partition("=")
        if not sep:
            sys.exit(f"mkinitrd: expected SOURCE=DEST, got {mapping!r}")
        dest = dest.strip("/")
        if os.path.isdir(source):
            for root, dirs, files in os.walk(source):
                dirs.sort()
                rel = os.path.relpath(root, source).replace(os.sep, "/")
                base = dest if rel == "." else (f"{dest}/{rel}" if dest else rel)
                if base:
                    add_parents(base + "/x")
                    entries.setdefault(base, None)
                for name in sorted(files):
                    target = f"{base}/{name}" if base else name
                    add_parents(target)
                    entries[target] = os.path.join(root, name)
        elif os.path.isfile(source):
            add_parents(dest)
            entries[dest] = source
        else:
            sys.exit(f"mkinitrd: {source}: no such file or directory")
    return entries


def write_tar(output, entries):
    with tarfile.open(output, "w", format=tarfile.USTAR_FORMAT) as tar:
        for path in sorted(entries):
            source = entries[path]
            info = tarfile.TarInfo(path)
            info.uid = info.gid = owner_for(path)
            info.uname = info.gname = "root" if info.uid == 0 else path.split("/")[1]
            info.mode = mode_for(path, source is None)
            if source is None:
                info.type = tarfile.DIRTYPE
                info.mtime = BUILD_TIME   # folders: when the image was made
                tar.addfile(info)
            else:
                info.size = os.path.getsize(source)
                info.mtime = int(os.path.getmtime(source)) + LOCAL_OFFSET
                with open(source, "rb") as f:
                    tar.addfile(info, f)


def write_sysroot(directory, entries):
    shutil.rmtree(directory, ignore_errors=True)
    os.makedirs(directory)
    for path in sorted(entries):
        target = os.path.join(directory, path)
        if entries[path] is None:
            os.makedirs(target, exist_ok=True)
        else:
            # Contents only: WSL can't set timestamps or modes on /mnt/c files.
            shutil.copyfile(entries[path], target)


def main(argv):
    args = argv[1:]
    sysroot = None
    if "--sysroot" in args:
        i = args.index("--sysroot")
        sysroot = args[i + 1]
        del args[i:i + 2]
    if not args:
        sys.exit(__doc__)
    output, mappings = args[0], args[1:]
    entries = collect(mappings)
    write_tar(output, entries)
    if sysroot:
        write_sysroot(sysroot, entries)
    files = sum(1 for s in entries.values() if s is not None)
    print(f"mkinitrd: {output}: {files} files, {len(entries) - files} directories")


if __name__ == "__main__":
    main(sys.argv)
