"""Mark ELF executables as Linux programs (EI_OSABI = 3, ELFOSABI_LINUX), so viralKernel
gives them the Linux system call interface (src/proc/linux.c):  python3 mark_linux.py FILE..."""
import sys

for path in sys.argv[1:]:
    with open(path, "r+b") as f:
        head = f.read(16)
        if head[:4] != b"\x7fELF" or head[4] != 2:
            sys.exit(f"{path}: not a 64-bit ELF file")
        f.seek(7)
        f.write(bytes([3]))
