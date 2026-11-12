// SPDX-License-Identifier: GPL-2.0
/*
 * Partial decoding returns what it could decode (not an error) when the
 * input ends early; the point is that it must not read past the input.
 *
 * Userspace regression test for LZ4_decompress_safe_partial(): the
 * compressed input must never be read past its end. Builds
 * lib/lz4/lz4_decompress.c against the stubs in shim/; see the Makefile.
 *
 * Each input is copied to the very end of a mapping that is followed by a
 * PROT_NONE page, so any over-read faults instead of passing silently.
 */
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/lz4.h>

static int failures;

static int run(const char *name, const void *in, int inlen, int target,
	       int cap, int want_ok, int want_len, const char *want)
{
	long pg = sysconf(_SC_PAGESIZE);
	char *map = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	char *src = map + pg - inlen;
	char dst[64];
	int ret;

	mprotect(map + pg, pg, PROT_NONE);
	memcpy(src, in, inlen);
	memset(dst, 0, sizeof(dst));

	ret = LZ4_decompress_safe_partial(src, dst, inlen, target, cap);
	if (want_ok ? (ret != want_len || memcmp(dst, want, want_len))
		    : ret >= 0) {
		printf("FAIL %s: ret %d\n", name, ret);
		failures++;
	} else {
		printf("ok   %s: ret %d\n", name, ret);
	}
	munmap(map, 2 * pg);
	return ret;
}

int main(void)
{
	/* 5 literals, then input ends where a match offset should be */
	static const char lit_end[] = "\x50" "abcde";
	/* 5 literals + a complete offset, but nothing after it */
	static const char no_mlen[] = "\x50" "abcde\x05\x00";
	/* 5 literals + 1 offset byte only */
	static const char half_off[] = "\x50" "abcde\x05";
	/* 4 literals, match of 4 at offset 4, then 5 trailing literals */
	static const char full[] =
		"\x40" "abcd" "\x04\x00" "\x50" "efghi";
	/* literal-run length byte missing */
	static const char no_ext[] = "\xf0";

	/* the reported case: srcSize 6, target 8, capacity 8 */
	run("literals end input, target past it (reported case)", lit_end, 6, 8, 8, 1, 5, "abcde");
	run("same, target 5 (satisfied)", lit_end, 6, 5, 8, 1, 5, "abcde");
	run("same, target 3 (stop inside literals)", lit_end, 6, 3, 8,
	    1, 3, "abc");
	run("offset cut to 1 byte", half_off, 7, 8, 8, 1, 5, "abcde");
	run("offset ends the input", no_mlen, 8, 12, 16, 1, 5, "abcde");
	run("short output buffer", full, 13, 8, 4, 1, 4, "abcd");
	run("full stream, partial target 7", full, 13, 7, 16, 1, 7, "abcdabc");
	run("full stream, target 13", full, 13, 13, 16, 1, 13, "abcdabcdefghi");
	run("truncated literal length", no_ext, 1, 8, 16, 0, 0, NULL);
	run("empty output", full, 13, 0, 0, 0, 0, NULL);
	return failures ? 1 : 0;
}
