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

Without `--cross` only `hrsblob.S`, `roundtrip.ld` and `report.txt` are
written, and nothing is assembled or checked. With it the tool also assembles,
links at the stock addresses and verifies.

## Method

1. Kernel is taken from the boot image. Symbols come from kallsyms. The
   address table is ABS64-relocated against `_text`, so addresses are read from
   the RELA addends. Offsets are relative to `_text`.
2. Function bounds are the kallsyms symbols in `[0x4d4794, 0x4eae14)` minus the
   two excluded timer functions. Expected names at fixed offsets are checked, so
   a different image fails loudly.
3. Per-function forward dataflow (own AArch64 decoder, no disassembler) tracks
   registers holding addresses (ADRP, ADD imm, MOV, ADD reg). It records every
   load/store with its exact address and width, escapes through calls, and
   register-indexed accesses. 8-byte loads from read-only memory that hold
   link-time pointers (the ABS64 table) are folded, so accesses through them are
   attributed to the target object. GCC jump tables (`adr; ldr; add; br`) are
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
