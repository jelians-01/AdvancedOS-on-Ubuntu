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

clean:
	rm -rf build

.PHONY: all exception timer return context memory host-memory build clean
