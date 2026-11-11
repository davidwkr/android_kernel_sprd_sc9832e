# Android 17 userspace: kernel features (4.14)

Reference: the Xiaomi apollo 4.19 kernel (`kernel_xiaomi_apollo`, HEAD
3b4b0017b71f) that boots Android 17. Config lives in
`arch/arm64/configs/watch_414_a17.fragment`; merge it after
`sprd_sharkle_defconfig` + `watch_414.fragment` (+ `watch_414_adf.fragment`),
then `olddefconfig`.

## EROFS + incfs (branch a17/erofs-incfs)

### Backported

| Commit subject | Source |
|---|---|
| locking/barriers: Introduce smp_cond_load_relaxed() and atomic_cond_read_relaxed() | upstream fcfdfe30e324 (v4.17), trimmed (no atomic_long_ variants) |
| include/linux/pagemap.h: introduce attach/detach_page_private | upstream (v5.8) via apollo 1d696af89e90, Guoqing Jiang |
| fs: add EROFS read-only filesystem (with LZ4 decompression) | apollo `fs/erofs` + `trace/events/erofs.h`, upstream v5.10 feature set |
| block: add bio_first_page_all() | upstream v4.13+ |
| fs-verity: add the fs/verity core | apollo `fs/verity`, uapi/fsverity.h (upstream v5.4 series + fixes) |
| stddef: Introduce sizeof_field() | upstream v5.4 |
| locking/rwsem: Add down_read_killable() | upstream 76f8507f7a64 (v4.16), asm-generic part only |
| ANDROID: Incremental fs: add the Incremental File System | apollo `fs/incfs` + uapi/incrementalfs.h |

EROFS and incfs are imported as final trees: 4.14 has neither, so the
47 (EROFS) / 183 (incfs) individual apollo commits cannot be applied one by
one. Authors are in apollo's `git log -- fs/erofs` / `fs/incfs`.

4.14 adaptations (everything else is verbatim):
- EROFS: own `workstn_lock` (4.14 radix trees have no `xa_lock`);
  `<linux/overflow.h>` for `struct_size()`.
- fs-verity: `mempool_create_kmalloc_pool()` (no `mempool_init*()`),
  `KMEM_CACHE()` (no usercopy whitelist), `inode_permission()`,
  `FIELD_SIZEOF()`.
- incfs: `__poll_t`/`EPOLL*` -> `unsigned int`/`POLL*`, `ksys_close` ->
  `sys_close`, `vm_fault_t` -> `int`, `ZSTD_DECOMPRESS`/`LZ4_DECOMPRESS`
  selects; one unused local dropped.
- UAPI added exactly as upstream: `FS_IOC_{ENABLE,MEASURE,READ_METADATA}_VERITY`
  (133/134/135), `struct fsverity_*`, `S_VERITY` 65536, `FS_VERITY_FL`,
  `STATX_ATTR_VERITY`, `EROFS_SUPER_MAGIC_V1`, `incrementalfs.h` ioctls.

### Config

```
CONFIG_EROFS_FS=y  _XATTR _POSIX_ACL _SECURITY _ZIP (LZ4)
CONFIG_FS_VERITY=y  CONFIG_FS_VERITY_BUILTIN_SIGNATURES=y
CONFIG_INCREMENTAL_FS=y
```

### What remains / limits

- No filesystem provides `fsverity_operations` (ext4/f2fs hooks not
  backported); fs-verity is only used through incfs. Android's
  `/data` fs-verity (APK verity on f2fs/ext4) therefore still fails with
  `ENOTTY`/`EOPNOTSUPP`.
- `EROFS_FS_PCPU_KTHREAD` (off) was not build-tested.
- `down_read_killable()` is only implemented for the asm-generic rwsem
  (arm64); other architectures would not build from this tree.
- The in-tree `lib/lz4` (v1.8.3 port) is kept, apollo uses lz4 v1.10.0;
  upstream EROFS ran on the old library until v5.19.
- The `.fs-verity` keyring needs the signing certificate loaded by
  userspace (init) for `BUILTIN_SIGNATURES` to accept v4 APK signatures.
- Not run on hardware yet; only compile-tested.

### Testing on a device

1. `zcat /proc/config.gz | grep -E 'EROFS|INCREMENTAL|FS_VERITY'`;
   `grep -E 'erofs|incremental' /proc/filesystems`.
2. EROFS: build an image on the host
   (`mkfs.erofs -zlz4hc img.erofs dir`, and one without `-z`), then
   `mount -t erofs -o ro,loop img.erofs /mnt`; compare `find`/`sha256sum`
   against the source dir, check `ls -Z`/`getfattr -d` for xattrs and
   `dmesg | grep erofs` for errors. Boot with `system`/`vendor` as EROFS.
3. incfs: `ls /sys/fs/incremental-fs/features` (expect `corefs`, `v2`,
   `zstd`, ...); `adb install --incremental app.apk` with an APK v4
   signature (needs the Android incremental service); or
   `mount -t incremental-fs` via the incfs selftests from the Android tree
   (`tools/testing/selftests/filesystems/incfs`).
4. Check `dmesg` for lockdep/KASAN reports while reading compressed
   EROFS files under memory pressure.
