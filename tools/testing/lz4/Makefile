# SPDX-License-Identifier: GPL-2.0
# Host build of lib/lz4/lz4_decompress.c with stubbed kernel headers.
# Usage: make -C tools/testing/lz4 run [OUT=dir]
SRC := ../../..
OUT ?= .
CFLAGS += -O2 -g -Wall -Ishim -I$(SRC)/include -I$(SRC)/lib/lz4

$(OUT)/lz4_partial_test: lz4_partial_test.c $(SRC)/lib/lz4/lz4_decompress.c
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ lz4_partial_test.c \
		$(SRC)/lib/lz4/lz4_decompress.c

run: $(OUT)/lz4_partial_test
	$(OUT)/lz4_partial_test

.PHONY: run
