CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
BUILD := build
READER := $(BUILD)/fat32_reader

.PHONY: all test clean

all: $(READER)

$(BUILD):
	mkdir -p $@

$(READER): fat32_reader.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

test: $(READER)
	python3 test_fat32.py ./$(READER) "$$(command -v mkfs.fat)"

clean:
	rm -rf $(BUILD)
