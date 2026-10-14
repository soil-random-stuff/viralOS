# viralOS - the viralKernel, the DDE desktop and their programs
# Build inside WSL/Linux:  make        -> viralos.iso
#                          make run    -> boot in QEMU (window + serial on terminal)

KERNEL := build/kernel.elf
INITRD := build/initrd.tar
ISO    := viralos.iso

CC := gcc
LD := ld

CFLAGS := -g -O2 -pipe -std=gnu11 -Wall -Wextra \
	-ffreestanding -fno-stack-protector -fno-stack-check -fno-PIC -fno-PIE \
	-fno-omit-frame-pointer -fno-tree-loop-distribute-patterns \
	-ffunction-sections -fdata-sections \
	-m64 -march=x86-64 -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
	-mno-red-zone -mcmodel=kernel
CPPFLAGS := -Isrc -Iabi -Ilimine-protocol/include -MMD -MP
LDFLAGS  := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
	--gc-sections -T linker.ld

SRCS := $(shell find src -name '*.c' -o -name '*.S')
OBJS := $(SRCS:src/%=build/obj/%.o) build/obj/font.o

QEMU      := qemu-system-x86_64
# -cpu max: QEMU's default CPU model has no AVX.
# -rtc base=localtime: the clock on the panel shows your time, not UTC.
# (No -no-reboot: restarting from the desktop should restart, not quit QEMU.)
# Sound: an HD Audio card playing through PulseAudio (WSLg provides it); AUDIO=wav records
# to build/audio.wav instead, AUDIO=none keeps quiet.
AUDIO     ?= pa
comma     := ,
AUDIODEV  := $(if $(filter wav,$(AUDIO)),wav$(comma)path=build/audio.wav,$(AUDIO))
# Network: QEMU's built-in NAT (10.0.2.x, DHCP, DNS at 10.0.2.3) behind the card NIC names:
# virtio-net-pci (default), e1000, e1000e. NIC=none leaves the machine without one.
NIC       ?= virtio-net-pci
NETFLAGS  := $(if $(filter none,$(NIC)),-nic none,-netdev user$(comma)id=net0 -device $(NIC)$(comma)netdev=net0)
QEMUFLAGS := -M q35 -cpu max -smp 4 -m 512M -rtc base=localtime -cdrom $(ISO) \
	-audiodev $(AUDIODEV),id=snd0 -device ich9-intel-hda -device hda-output,audiodev=snd0 $(NETFLAGS)

.PHONY: all run run-sdl run-headless run-uefi debug screenshot clean distclean

all: $(ISO)

# --- third-party: Limine bootloader binaries + protocol header ---------------

limine/limine-bios.sys:
	git clone --depth=1 --branch=v11.x-binary https://github.com/limine-bootloader/limine.git limine

limine/limine: limine/limine-bios.sys
	$(MAKE) -C limine limine

limine-protocol/include/limine.h:
	git clone --depth=1 https://github.com/limine-bootloader/limine-protocol.git limine-protocol

# --- kernel -----------------------------------------------------------------

build/obj/%.c.o: src/%.c limine-protocol/include/limine.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# lwIP (src/net/lwip, unchanged but for one hook in api/sockets.c) and the code that uses it see
# lwIP's headers and its port (src/net/port); the rest of the kernel only sees net/net.h.
build/obj/net/%.c.o: CPPFLAGS += -Isrc/net/port -Isrc/net/port/libc -Isrc/net/lwip/include
build/obj/net/lwip/%.c.o: CFLAGS += -Wno-unused-parameter -Wno-sign-compare -Wno-address

# lwext4 (src/fs/ext4/lwext4, unchanged), the read-write ext4 driver's engine: its libc is in
# src/fs/ext4/port, its settings here (its own errno and open flags, no debug output, 8 filesystems
# at once). Only fs/ext4/ sees its headers.
LWEXT4_CONFIG := -DCONFIG_USE_DEFAULT_CFG=1 -DCONFIG_HAVE_OWN_ERRNO=1 -DCONFIG_HAVE_OWN_OFLAGS=1 \
	-DCONFIG_HAVE_OWN_ASSERT=1 -DCONFIG_DEBUG_PRINTF=0 -DCONFIG_DEBUG_ASSERT=0 -DCONFIG_BLOCK_DEV_ENABLE_STATS=0 \
	-DCONFIG_BLOCK_DEV_CACHE_SIZE=256 -DCONFIG_EXT4_BLOCKDEVS_COUNT=8 -DCONFIG_EXT4_MOUNTPOINTS_COUNT=8
build/obj/fs/ext4/%.c.o: CPPFLAGS += -Isrc/fs/ext4/port/libc -Isrc/fs/ext4/lwext4/include -Isrc/fs/ext4 $(LWEXT4_CONFIG)
build/obj/fs/ext4/lwext4/%.c.o: CFLAGS += -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function \
	-Wno-unused-variable -Wno-implicit-fallthrough -Wno-unused-but-set-variable

build/obj/%.S.o: src/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# The console font is linked in as a raw blob (_binary_terminus16_psf_start/_end).
build/obj/font.o: fonts/terminus16.psf
	@mkdir -p $(dir $@)
	cd fonts && objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		terminus16.psf ../$@

.SECONDARY:   # keep intermediate user objects

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

# --- user programs -------------------------------------------------------------

# Every user/programs/NAME.c becomes /usr/bin/NAME, using the shared libraries in /usr/lib: the C
# library (user/lib -> libos.so) and user/libs/NAME/ -> libNAME.so (graphics etc.). Programs are
# position-independent executables that name the libraries they use; the kernel loads those when
# a program starts (src/proc/dynlink.c), sharing each library's code between processes.
# Programs may use x87/SSE/SSE2 freely (the kernel saves them per process);
# AVX only in functions marked target("avx") after checking for it.
# -fno-math-errno lets sqrt() become a single instruction.
USER_CFLAGS_COMMON := -g -O2 -pipe -std=gnu11 -Wall -Wextra \
	-ffreestanding -fno-stack-protector \
	-fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns \
	-ffunction-sections -fdata-sections -fno-math-errno \
	-m64 -march=x86-64
USER_CFLAGS     := $(USER_CFLAGS_COMMON) -fPIE
USER_PIC_CFLAGS := $(USER_CFLAGS_COMMON) -fPIC
USER_CPPFLAGS := -Iuser/include -Iabi -MMD -MP
# (sysv hash tables: what the kernel's dynamic linker reads)
USER_LDFLAGS  := -m elf_x86_64 -nostdlib -z max-page-size=0x1000 -z noexecstack --hash-style=sysv \
	--gc-sections --strip-debug
USER_PROG_LDFLAGS := $(USER_LDFLAGS) -pie --no-dynamic-linker
USER_LIB_LDFLAGS  := $(USER_LDFLAGS) -shared -Bsymbolic --no-undefined

LIBDIR := build/user/Library

# The libraries: user/lib, and user/libs/*/. Their code is built position-independent (build/user/pic/).
USER_LIB_SRCS := $(wildcard user/lib/*.c)
USER_LIB_OBJS := $(USER_LIB_SRCS:user/%=build/user/pic/%.o)
USER_LIBS     := $(notdir $(patsubst %/,%,$(wildcard user/libs/*/)))
USER_LIBS_OBJ  = $(patsubst user/%,build/user/pic/%.o,$(wildcard user/libs/$(1)/*.c) $(LIB_EXTRA_SRCS_$(1)))
USER_SOS      := $(LIBDIR)/libos.so $(USER_LIBS:%=$(LIBDIR)/lib%.so)
# What each library uses besides itself (recorded in it, so the kernel loads those too).
LIB_DEPS_gfx     := os
LIB_DEPS_ttf     := os gfx
LIB_DEPS_image   := os gfx
LIB_DEPS_audio   := os
LIB_DEPS_fileops := os image audio
LIB_DEPS_gui     := os gfx ttf image
LIB_DEPS_netconf := os
LIB_DEPS_users   := os gfx ttf image
LIB_DEPS_tls     := os
LIB_DEPS_http    := os tls
# libtls is mbedTLS (vendored, user/libs/tls/mbedtls) under a small API (tls.c, <tls.h>).
LIB_EXTRA_SRCS_tls := $(wildcard user/libs/tls/mbedtls/library/*.c)
build/user/pic/libs/tls/%.c.o: USER_CPPFLAGS += -Iuser/libs/tls -Iuser/libs/tls/mbedtls/include -DMBEDTLS_USER_CONFIG_FILE='"tls_config.h"'
# (the compiler is Linux's: without these mbedTLS would expect glibc)
build/user/pic/libs/tls/mbedtls/%.c.o: USER_PIC_CFLAGS += -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers \
	-U__linux__ -U__linux -Ulinux -U__unix__ -U__unix -Uunix -U__gnu_linux__
# A program names only the libraries it uses (--as-needed).
USER_LINK     := -L$(LIBDIR) --as-needed -los $(USER_LIBS:%=-l%)
USER_CRT0     := build/user/lib/crt0.S.o

# A program is either user/programs/NAME.c or a folder user/programs/NAME/
# whose .c files are linked together.
USER_SINGLE   := $(basename $(notdir $(wildcard user/programs/*.c)))
USER_MULTI    := $(notdir $(patsubst %/,%,$(wildcard user/programs/*/)))
USER_PROGRAMS := $(USER_SINGLE) $(USER_MULTI)
USER_BINS     := $(USER_PROGRAMS:%=build/user/bin/%)
USER_MULTI_OBJS = $(patsubst user/%,build/user/%.o,$(wildcard user/programs/$(1)/*.c))

build/user/pic/%.c.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_PIC_CFLAGS) $(USER_CPPFLAGS) -c $< -o $@

build/user/%.c.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(USER_CPPFLAGS) -c $< -o $@

build/user/%.S.o: user/%.S
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) $(USER_CPPFLAGS) -c $< -o $@

$(LIBDIR)/libos.so: $(USER_LIB_OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(USER_LIB_LDFLAGS) -soname libos.so $(USER_LIB_OBJS) -o $@

define USER_LIBRARY
$(LIBDIR)/lib$(1).so: $(call USER_LIBS_OBJ,$(1)) $(LIB_DEPS_$(1):%=$(LIBDIR)/lib%.so)
	@mkdir -p $$(dir $$@)
	$$(LD) $$(USER_LIB_LDFLAGS) -soname lib$(1).so $(call USER_LIBS_OBJ,$(1)) -L$(LIBDIR) $(LIB_DEPS_$(1):%=-l%) -o $$@
endef
$(foreach l,$(USER_LIBS),$(eval $(call USER_LIBRARY,$(l))))

# crt0 goes first so _start is at the beginning of .text.
build/user/bin/%: build/user/programs/%.c.o $(USER_CRT0) $(USER_SOS)
	@mkdir -p $(dir $@)
	$(LD) $(USER_PROG_LDFLAGS) -e _start $(USER_CRT0) $< $(USER_LINK) -o $@

define MULTI_PROGRAM
build/user/bin/$(1): $(call USER_MULTI_OBJS,$(1)) $(USER_CRT0) $(USER_SOS)
	@mkdir -p $$(dir $$@)
	$$(LD) $$(USER_PROG_LDFLAGS) -e _start $(USER_CRT0) $(call USER_MULTI_OBJS,$(1)) $(USER_LINK) -o $$@
endef
$(foreach p,$(USER_MULTI),$(eval $(call MULTI_PROGRAM,$(p))))

# --- root filesystem image ---------------------------------------------------

# The image is assembled from several places (scripts/mkinitrd.py has the
# folder layout and permission rules; abi/paths.h what each folder is for):
#   rootfs/            hand-written files: /etc, /home, launchers, docs
#   resources/         the desktop's fonts, icons, sounds... -> /usr/share/dde-res
#   build/user/bin/    compiled programs        -> /usr/bin
#   build/user/Library/ shared libraries        -> /usr/lib
# build/sysroot/ gets a plain copy of the result, to look at.
# Make can't list files whose names have spaces or brackets, so rootfs/ and
# resources/ are tracked through a fingerprint of every path, size and time instead.
ROOTFS_STATE := $(shell find rootfs resources -printf '%p %s %T@\n' | sort | md5sum | cut -d' ' -f1)

build/rootfs.stamp: FORCE
	@mkdir -p build
	@echo '$(ROOTFS_STATE)' | cmp -s - $@ || echo '$(ROOTFS_STATE)' > $@

.PHONY: FORCE

# ports/bin: Linux programs (the disk tools) built by scripts/build_ports.sh; kept out of build/ because
# building them takes a while. They run through the kernel's Linux layer (src/proc/linux.c).
PORT_BINS := $(wildcard ports/bin/*)

# /boot has what the installer puts on a disk to start it: the kernel and Limine's BIOS and UEFI parts.
BOOT_FILES := $(KERNEL) limine/limine-bios.sys limine/BOOTX64.EFI

$(INITRD): build/rootfs.stamp $(USER_BINS) $(USER_SOS) $(PORT_BINS) $(BOOT_FILES) scripts/mkinitrd.py
	@mkdir -p build
	python3 scripts/mkinitrd.py $@ rootfs=/ resources=/usr/share/dde-res ports/bin=/usr/bin \
		build/user/bin=/usr/bin $(LIBDIR)=/usr/lib $(KERNEL)=/boot/kernel.elf \
		limine/limine-bios.sys=/boot/limine/limine-bios.sys limine/BOOTX64.EFI=/boot/limine/BOOTX64.EFI \
		--sysroot build/sysroot

# --- bootable ISO (BIOS + UEFI) ---------------------------------------------

$(ISO): $(KERNEL) $(INITRD) limine.conf limine/limine
	rm -rf build/iso_root
	mkdir -p build/iso_root/boot/limine build/iso_root/EFI/BOOT
	cp $(KERNEL) build/iso_root/boot/kernel.elf
	cp $(INITRD) build/iso_root/boot/initrd.tar
	cp limine.conf limine/limine-bios.sys limine/limine-bios-cd.bin \
		limine/limine-uefi-cd.bin build/iso_root/boot/limine/
	cp limine/BOOTX64.EFI limine/BOOTIA32.EFI build/iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		build/iso_root -o $@ 2>/dev/null
	./limine/limine bios-install $@

# --- running ----------------------------------------------------------------

# A GTK window (shown through WSLg on Windows). `make run-sdl` uses SDL instead.
run: $(ISO)
	$(QEMU) $(QEMUFLAGS) -display gtk -serial stdio

run-sdl: $(ISO)
	$(QEMU) $(QEMUFLAGS) -display sdl -serial stdio

run-headless: $(ISO)
	$(QEMU) $(QEMUFLAGS) -serial stdio -display none

run-uefi: $(ISO)
	$(QEMU) -bios /usr/share/qemu/OVMF.fd $(QEMUFLAGS) -serial stdio

# Waits for GDB on :1234 (gdb build/kernel.elf -ex 'target remote :1234').
debug: $(ISO)
	$(QEMU) $(QEMUFLAGS) -no-reboot -serial stdio -s -S -d int,cpu_reset -D build/qemu.log

# Headless boot that saves build/serial.log and build/screen.png.
screenshot: $(ISO)
	bash scripts/screenshot.sh $(ISO)

clean:
	rm -rf build $(ISO)

distclean: clean
	rm -rf limine limine-protocol

-include $(OBJS:.o=.d) $(USER_LIB_OBJS:.o=.d) $(USER_SINGLE:%=build/user/programs/%.c.d) \
	$(foreach p,$(USER_MULTI),$(patsubst %.o,%.d,$(call USER_MULTI_OBJS,$(p)))) \
	$(foreach l,$(USER_LIBS),$(patsubst %.o,%.d,$(call USER_LIBS_OBJ,$(l)))) build/user/lib/crt0.S.d
