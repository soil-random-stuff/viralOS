"""Copy a folder tree into an NTFS image without mounting it (no root needed):
   python3 ntfs_fill.py <image> <folder>
Calls libntfs-3g directly (the library ntfscp uses): ntfs_create for folders and files,
ntfs_attr_pwrite for the data. Set LD_LIBRARY_PATH if libntfs-3g isn't installed system-wide."""
import ctypes, os, stat, sys

lib = ctypes.CDLL("libntfs-3g.so.89")
P = ctypes.c_void_p
lib.ntfs_mount.restype = P
lib.ntfs_mount.argtypes = [ctypes.c_char_p, ctypes.c_ulong]
lib.ntfs_umount.argtypes = [P, ctypes.c_int]
lib.ntfs_pathname_to_inode.restype = P
lib.ntfs_pathname_to_inode.argtypes = [P, P, ctypes.c_char_p]
lib.ntfs_mbstoucs.argtypes = [ctypes.c_char_p, ctypes.POINTER(P)]
lib.ntfs_create.restype = P
lib.ntfs_create.argtypes = [P, ctypes.c_uint32, P, ctypes.c_uint8, ctypes.c_uint32]
lib.ntfs_attr_open.restype = P
lib.ntfs_attr_open.argtypes = [P, ctypes.c_uint32, P, ctypes.c_uint32]
lib.ntfs_attr_pwrite.restype = ctypes.c_int64
lib.ntfs_attr_pwrite.argtypes = [P, ctypes.c_int64, ctypes.c_int64, P]
lib.ntfs_attr_close.argtypes = [P]
lib.ntfs_inode_close.argtypes = [P]
lib.ntfs_set_char_encoding.argtypes = [ctypes.c_char_p]
lib.ntfs_set_ntfs_attrib.argtypes = [P, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int]
lib.ntfs_set_char_encoding(b"utf8")

AT_DATA = 0x80
AT_UNNAMED = ctypes.addressof(ctypes.c_uint16.in_dll(lib, "AT_UNNAMED"))

image, src = sys.argv[1], sys.argv[2]
vol = lib.ntfs_mount(image.encode(), 0)
if not vol:
    sys.exit("ntfs_mount failed")
# NVolSetCompression(vol), as ntfs-3g's "compression" mount option does: struct ntfs_volume starts
# with two pointers, then `unsigned long state`; NV_Compression is bit 6.
state = ctypes.c_ulong.from_address(vol + 16)
state.value |= 1 << 6


def create(dir_path, name, mode):
    parent = lib.ntfs_pathname_to_inode(vol, None, dir_path.encode())
    if not parent:
        sys.exit(f"no folder {dir_path}")
    uname = P()
    n = lib.ntfs_mbstoucs(name.encode(), ctypes.byref(uname))
    ni = lib.ntfs_create(parent, 0, uname, n, mode)
    lib.ntfs_inode_close(parent)
    if not ni:
        sys.exit(f"ntfs_create {dir_path}/{name} failed")
    return ni


def copy(src_dir, dst_dir):
    for name in sorted(os.listdir(src_dir)):
        path = os.path.join(src_dir, name)
        dst = (dst_dir.rstrip("/") + "/" + name)
        if os.path.isdir(path):
            ni = create(dst_dir, name, stat.S_IFDIR)
            if name == "Compressed":
                # A compressed folder (FILE_ATTRIBUTE_DIRECTORY | COMPRESSED): files created in it are
                # written LZNT1-compressed (libntfs-3g only lets folders change this).
                if lib.ntfs_set_ntfs_attrib(ni, (0x810).to_bytes(4, "little"), 4, 0) != 0:
                    sys.exit(f"compressing {dst} failed")
            lib.ntfs_inode_close(ni)
            copy(path, dst)
        else:
            ni = create(dst_dir, name, stat.S_IFREG)
            data = open(path, "rb").read()
            if data:
                na = lib.ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0)
                buf = ctypes.create_string_buffer(data, len(data))
                if lib.ntfs_attr_pwrite(na, 0, len(data), buf) != len(data):
                    sys.exit(f"writing {dst} failed")
                lib.ntfs_attr_close(na)
            lib.ntfs_inode_close(ni)


copy(src, "/")
if lib.ntfs_umount(vol, 0) != 0:
    sys.exit("ntfs_umount failed")
