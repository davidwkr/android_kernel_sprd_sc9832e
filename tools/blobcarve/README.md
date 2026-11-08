# blobcarve

Carves a relocatable AArch64 ELF object (as assembly plus `.o`) out of a
stripped arm64 kernel Image: the hx3918 hardware layer and the tyhx_*
algorithm of the stock HRS driver, without the glue.

    python3 -I tools/blobcarve/blobcarve.py \
        --boot-img /path/to/stock/boot.img \
        --out /path/to/outdir \
        --cross /path/to/aarch64-linux-gnu-

Python 3, stdlib only, single file (`-I` does not add the script directory to
`sys.path`). Outputs go to `--out` only. Never commit anything from `--out`
or the input image: they contain stock bytes.

Per-device usage (same command, only the image changes; the function set and
all addresses are derived per image; `--out` must be a fresh directory per
image):

    # DM63 stock boot.img
    python3 -I tools/blobcarve/blobcarve.py --boot-img <DM63>/boot.img \
        --out /tmp/blob-dm63 --cross /usr/bin/aarch64-linux-gnu-
    # DW100U stock boot.img
    python3 -I tools/blobcarve/blobcarve.py --boot-img <DW100U>/boot.img \
        --out /tmp/blob-dw100u --cross /usr/bin/aarch64-linux-gnu-
    # MT107 stock boot image (a bare kernel Image works too)
    python3 -I tools/blobcarve/blobcarve.py --boot-img <MT107>/boot.bin \
        --out /tmp/blob-mt107 --cross /usr/bin/aarch64-linux-gnu-

Each run prints the function-set summary and ends with
`RESULT: BYTE-IDENTICAL`; any fatal check exits with status 2 and writes no
usable result. The blob is image specific (addresses, data layout, and for
DW100U some logic differ): link the `.o` produced from the image of the
device you build for.

Comparing two images (no toolchain needed, writes `diff_report.txt`):

    python3 -I tools/blobcarve/blobcarve.py --boot-img <A>/boot.img \
        --diff-with <B>/boot.img --out /tmp/blob-diff

The report lists functions present in only one image, per function whether
the normalised code is identical, differs only in constants/tables/strings
or in logic (address-bearing words are compared by object name, offset,
string text or callee name, so layout shifts do not count), the undefined
symbol sets and per-function calls into them, the vendor struct field
offsets, and the carved data objects.

Without `--cross` only `hrsblob.S`, `roundtrip.ld` and `report.txt` are
written, and nothing is assembled or checked. With it the tool also assembles,
links at the stock addresses and verifies.

## Method

1. Kernel is taken from the boot image. Symbols come from kallsyms. The
   address table is ABS64-relocated against `_text`, so addresses are read from
   the RELA addends. Offsets are relative to `_text`.
2. The function set is chosen by name, nothing is addressed by constant.
   The range starts at the lowest `hx3918_*` text symbol and ends at the
   symbol that follows the highest `hx3918_*`/`tyhx_*` text symbol. Every text
   symbol inside is carved (hardware layer, algorithm and the unprefixed
   helpers between them, e.g. `read_data_packet`, `hr_sqrt`) except
   `hx3918_ppg_timer_cfg` and `hx3918_agc_timer_cfg`. Fatal: either excluded
   function missing, a non-text or `hrs3xxx_*` (glue) symbol inside the range,
   two functions at one address, or gaps that are not exactly the excluded
   functions. The `_text` symbol must sit at offset 0 and the ABS64 relocation
   symbol is derived as the dominant one (ambiguity is fatal). The only
   remaining constant is the kernel link base `--text-va` (default
   `0xffffff8008080000`, identical for all UNISOC 4.4.83 images seen so far);
   a wrong value is caught by the `_text` check. Pointer-looking bounds come
   from the highest kallsyms address.
3. Per-function forward dataflow (own AArch64 decoder, no disassembler) tracks
   registers holding addresses (ADRP, ADD imm, MOV, ADD reg). It records every
   load/store with its exact address and width, escapes through calls, and
   register-indexed accesses. 8-byte loads from read-only memory that hold
   link-time pointers (the ABS64 table) are folded, so accesses through them are
   attributed to the target object. A load of the `hrs3xxx_t` global (the
   pointer to the vendor driver struct) yields a tracked struct pointer and
   every access through it is recorded as a field offset (used by the diff
   report only; it never changes the carve). GCC jump tables (`adr; ldr; add; br`) are
   recognised and bounded by the preceding `cmp`.
4. Chunks are page-preserving. Code words stay as `.inst`. Only these change:
   - ADRP: `adrp xN, <chunk>+<page delta>`, and the lo12 immediates stay valid
     because each chunk section is `.balign 4096` with the stock in-page offset
     kept as leading padding;
   - BL and tail B: `bl <symbol>`;
   - 8-aligned data qwords holding kernel VAs: `.quad <chunk>+off` (the
     8-entry pointer table).
5. Chunk extents come from exact accesses. Addresses with no static bound
   (escaped pointers, register-indexed bases, pointer-table targets) are
   extended to the end of the containing kallsyms data object, and the tool
   dies if that would exceed 16 KiB. Strings are extended to the NUL. Chunks of
   the same memory class within 255 bytes are merged, and chunks sharing an ADRP
   are merged. The report shows both the static end and the final end.
6. Checks, all fatal: unknown instruction classes (SIMD/FP, atomics), any
   PC-relative branch, ADR or literal load that leaves its function, tail calls
   or BLs to non-symbol addresses, unreachable PC-relative words, unresolved
   `br`, pointer-looking data without a home, unaligned VA-looking qwords.

## External variables (deviation from "GOT slots")

`__stack_chk_guard` is not a GOT slot in this image. The code does
`adrp; ldr x, [x, #lo]` and uses the loaded value directly, so a `.quad &sym`
slot would hand the code an address where it expects the value. Those
references are therefore rewritten symbolically (`adrp xN, sym` and
`ldr xt, [xn, :lo12:sym]`), and the symbol is left undefined. `system_wq` is
configured the same way but the carved range does not reference it (only the
glue does).

## Validation (run by default with `--cross`)

- Round trip: link at the stock VAs with `roundtrip.ld`, externals resolved to
  their stock addresses, and compare every code section and every copied
  data chunk with the stock bytes. bss chunks are checked for address and size.
- Layout independence: relink with chunks moved by +0x40000 and the external
  variable by a non page-multiple; every ADRP, the external LDRs and data
  pointer slots must change by exactly the shift, and no other word may change.
- The undefined set from `objdump -t` must equal the emitter's set.

## Exports

`hrsblob_hrs3xxx_t`, `hrsblob_alg_ram`, `hrsblob_spo2_alg_data` point at those
objects inside the chunks. Every carved function is global under its kallsyms
name.

## Verified results

All run with `--cross`, 110 functions each (51 `hx3918_*`, 41 `tyhx_*`, 18
helpers), 10 undefined symbols, 8 folded pointer slots, 6 jump tables:

| image | round trip | layout independence |
|-------|-----------|---------------------|
| DM63 stock boot.img | byte-identical | pass |
| DW100U stock boot.img (and a second DW100U image) | byte-identical | pass |
| MT107 stock boot image (and its extracted kernel Image) | byte-identical | pass |

Diff findings (see `--diff-with`): the algorithm (`tyhx_*`, `hr_sqrt`,
`spo2_sort_*`) is instruction-identical on all three. MT107 equals DM63 in the
hardware layer apart from one constant in `hx3918_spo2_read`. DW100U differs
from both in five layer functions (logic: `read_data_packet`,
`hx3918_hrs_ppg_Int_handle`, `hx3918_spo2_check_unwear`,
`hx3918_spo2_check_wear`, `hx3918_spo2_read`) plus a few tuning constants and
data tables. The undefined symbol set and the vendor struct field offsets used
by the layer (+0x00 and +0x130..+0x14c through the `hrs3xxx_t` pointer) are the
same on all three, so one kernel-side shim fits all, with the blob taken from
the matching image.
