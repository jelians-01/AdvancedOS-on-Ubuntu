SHELL := /bin/sh

CROSS ?= loongarch64-unknown-linux-gnu-
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump

CFLAGS := -march=loongarch64 -mabi=lp64d -ffreestanding -fno-pie \
          -fno-stack-protector -fno-asynchronous-unwind-tables -fno-builtin \
          -O2 -Wall -Wextra
LDFLAGS := -nostdlib -nostartfiles -nodefaultlibs -static -no-pie \
           -Wl,--build-id=none

all: build/loongarch-probe.elf build/loongarch-probe.bin build/loongarch-probe.dis

exception: build/exception-probe.elf build/exception-probe.dis

timer: build/timer-probe.elf build/timer-probe.dis

return: build/return-probe.elf build/return-probe.dis

context: build/context-probe.elf build/context-probe.dis

memory: build/memory-probe.elf build/memory-probe.dis

vm: build/vm-probe.elf build/vm-probe.dis

user: build/user-probe.elf build/user-probe.dis

disk: build/disk-probe.elf build/disk-reboot.elf build/disk-single.elf build/disk-error.elf

user-disk: build/user-disk-probe.elf

host-user: | build
	$(HOST_CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer -fno-pie -no-pie -fsanitize=address,undefined page-allocator.c vm-table.c user-copy.c user-copy-host-test.c -o build/user-copy-host-test
	./build/user-copy-host-test

host-vm: | build
	$(HOST_CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer -fno-pie -no-pie -fsanitize=address,undefined page-allocator.c vm-table.c vm-host-test.c -o build/vm-host-test
	./build/vm-host-test

HOST_CC ?= cc
host-memory: | build
	$(HOST_CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer -fno-pie -no-pie -fsanitize=address,undefined page-allocator.c memory-test.c memory-host-test.c -o build/memory-host-test
	./build/memory-host-test

build:
	mkdir -p $@

build/start.o: start.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/loongarch-probe.elf: build/start.o linker.ld
	$(CC) $(LDFLAGS) -Wl,-T,linker.ld build/start.o -o $@

build/loongarch-probe.bin: build/loongarch-probe.elf
	$(OBJCOPY) -O binary $< $@

build/loongarch-probe.dis: build/loongarch-probe.elf
	$(OBJDUMP) -d -S $< > $@

build/exception-start.o: exception-start.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/exception-vector.o: exception-vector.S | build
	$(CC) $(CFLAGS) -c $< -o $@

build/exception-probe.elf: build/exception-start.o build/exception-vector.o exception-linker.ld
	$(CC) $(LDFLAGS) -Wl,-T,exception-linker.ld build/exception-start.o build/exception-vector.o -o $@

build/exception-probe.dis: build/exception-probe.elf
	$(OBJDUMP) -d -S $< > $@

build/timer-start.o: timer-start.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/timer-vector.o: timer-vector.S | build
	$(CC) $(CFLAGS) -c $< -o $@

build/timer-probe.elf: build/timer-start.o build/timer-vector.o exception-linker.ld
	$(CC) $(LDFLAGS) -Wl,-T,exception-linker.ld build/timer-start.o build/timer-vector.o -o $@

build/timer-probe.dis: build/timer-probe.elf
	$(OBJDUMP) -d -S $< > $@

build/return-start.o: return-start.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/return-vector.o: return-vector.S | build
	$(CC) $(CFLAGS) -c $< -o $@

build/return-probe.elf: build/return-start.o build/return-vector.o return-linker.ld
	$(CC) $(LDFLAGS) -Wl,-T,return-linker.ld build/return-start.o build/return-vector.o -o $@

build/return-probe.dis: build/return-probe.elf
	$(OBJDUMP) -d -S $< > $@

build/context-start.o: context-start.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/context-entry.o: context-entry.S | build
	$(CC) $(CFLAGS) -c $< -o $@

build/context-switch.o: context-switch.S | build
	$(CC) $(CFLAGS) -c $< -o $@

build/context-probe.elf: build/context-start.o build/context-entry.o build/context-switch.o context-linker.ld
	$(CC) $(LDFLAGS) -Wl,-T,context-linker.ld build/context-start.o build/context-entry.o build/context-switch.o -o $@

build/context-probe.dis: build/context-probe.elf
	$(OBJDUMP) -d -S $< > $@

build/memory-entry.o: memory-entry.S | build
	$(CC) $(CFLAGS) -Werror -c $< -o $@

build/memory-start.o: memory-start.c page-allocator.h | build
	$(CC) $(CFLAGS) -Werror -c $< -o $@

build/page-allocator.o: page-allocator.c page-allocator.h | build
	$(CC) $(CFLAGS) -Werror -c $< -o $@

build/memory-test.o: memory-test.c page-allocator.h | build
	$(CC) $(CFLAGS) -Werror -c $< -o $@

build/memory-probe.elf: build/memory-entry.o build/memory-start.o build/page-allocator.o build/memory-test.o memory-linker.ld
	$(CC) $(LDFLAGS) -Wl,-T,memory-linker.ld $(filter %.o,$^) -o $@

build/memory-probe.dis: build/memory-probe.elf
	$(OBJDUMP) -d -S $< > $@

build/vm-%.o: vm-%.c vm-table.h page-allocator.h | build
	$(CC) $(CFLAGS) -Werror -c $< -o $@

build/vm-%.o: vm-%.S | build
	$(CC) $(CFLAGS) -Werror -c $< -o $@

build/vm-probe.elf: build/vm-entry.o build/vm-vector.o build/vm-start.o build/vm-table.o build/page-allocator.o memory-linker.ld
	$(CC) $(LDFLAGS) -Wl,-T,memory-linker.ld $(filter %.o,$^) -o $@

build/vm-probe.dis: build/vm-probe.elf
	$(OBJDUMP) -d -S $< > $@

build/user-%.o: user-%.c user-copy.h vm-table.h page-allocator.h | build
	$(CC) $(CFLAGS) -Werror -c $< -o $@

build/user-%.o: user-%.S | build
	$(CC) $(CFLAGS) -Werror -c $< -o $@

build/user-probe.elf: build/user-entry.o build/user-vector.o build/user-payload.o build/user-runtime.o build/user-copy.o build/vm-table.o build/page-allocator.o memory-linker.ld
	$(CC) $(LDFLAGS) -Wl,-T,memory-linker.ld $(filter %.o,$^) -o $@

build/user-probe.dis: build/user-probe.elf
	$(OBJDUMP) -d -S $< > $@

build/user-disk-runtime.o: user-runtime.c user-copy.h vm-table.h pci-block.h | build
	$(CC) $(CFLAGS) -Werror -DUSER_DISK=1 -c $< -o $@

build/user-disk-payload.o: user-payload.S | build
	$(CC) $(CFLAGS) -Werror -DUSER_DISK=1 -c $< -o $@

build/user-disk-probe.elf: build/user-entry.o build/user-vector.o build/user-disk-payload.o build/user-disk-runtime.o build/user-copy.o build/vm-table.o build/page-allocator.o build/pci-block.o build/probe-mem.o memory-linker.ld
	$(CC) $(LDFLAGS) -Wl,-T,memory-linker.ld $(filter %.o,$^) -o $@

build/disk-entry.o: disk-entry.S | build
	$(CC) $(CFLAGS) -Werror -c $< -o $@

build/pci-block.o: pci-block.c pci-block.h | build
	$(CC) $(CFLAGS) -Werror -c $< -o $@

build/disk-test.o: disk-test.c pci-block.h | build
	$(CC) $(CFLAGS) -Werror -c $< -o $@

build/disk-reboot-test.o: disk-test.c pci-block.h | build
	$(CC) $(CFLAGS) -Werror -DDISK_PHASE=2 -c $< -o $@

build/disk-single-test.o: disk-test.c pci-block.h | build
	$(CC) $(CFLAGS) -Werror -DEXPECTED_DISKS=1 -c $< -o $@

build/disk-error-test.o: disk-test.c pci-block.h | build
	$(CC) $(CFLAGS) -Werror -DDISK_IO_ERROR=1 -c $< -o $@

build/probe-mem.o: probe-mem.c | build
	$(CC) $(CFLAGS) -Werror -c $< -o $@

build/disk-probe.elf: build/disk-entry.o build/pci-block.o build/disk-test.o build/probe-mem.o memory-linker.ld
	$(CC) $(LDFLAGS) -Wl,-T,memory-linker.ld $(filter %.o,$^) -o $@

build/disk-reboot.elf: build/disk-entry.o build/pci-block.o build/disk-reboot-test.o build/probe-mem.o memory-linker.ld
	$(CC) $(LDFLAGS) -Wl,-T,memory-linker.ld $(filter %.o,$^) -o $@

build/disk-single.elf: build/disk-entry.o build/pci-block.o build/disk-single-test.o build/probe-mem.o memory-linker.ld
	$(CC) $(LDFLAGS) -Wl,-T,memory-linker.ld $(filter %.o,$^) -o $@

build/disk-error.elf: build/disk-entry.o build/pci-block.o build/disk-error-test.o build/probe-mem.o memory-linker.ld
	$(CC) $(LDFLAGS) -Wl,-T,memory-linker.ld $(filter %.o,$^) -o $@

clean:
	rm -rf build

.PHONY: all exception timer return context memory host-memory vm host-vm user host-user disk user-disk build clean
