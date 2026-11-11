# Android 17 kernel features on the 4.14 watch kernel

Reference: Xiaomi apollo 4.19 (`kernel_xiaomi_apollo`). Config lines live in
`arch/arm64/configs/watch_414_a17.fragment`.

## binderfs (`CONFIG_ANDROID_BINDERFS`)

Backported from the apollo/android-4.19 series, original authorship kept
(`UPSTREAM:` subjects, upstream SHAs in each commit message):

- 3ad20fe393b3 binder: implement binderfs, plus fdd94acd50d6, b6c770d7c9dc,
  849d540ddfcd (max option), c13295ad219d (binderfs.h), 36bdf3cae09d,
  211b64e4b5b6 (+ 01684db950ea, 36975fc3e5f2, 01b3f1fc5683, 4198479524ae,
  29ef1c8e16ae, da8ddba566ff, 7e7ca7744a53, 7fefaadd6a96, e98e6fa18636,
  7c4d08fc4d5a, 5b9633af298b): binderfs follow-up fixes
- ca2864c6e896, 028fb5822b76: default devices through binderfs, name validation
- f00834518ed3, 0e13e452dafc, c31e73121f4c, 4feb80faf428: `stats=global` mount
  option, stats/state/transactions/transaction_log/proc/<pid> files
- 2669b8b0c798 + f0fe2c0f050d "prevent UAF for binderfs devices" I and II,
  "prevent UAF read in print_binder_transaction_log_entry()", `__u32` device
  numbers (uapi `struct binderfs_device`, `BINDER_CTL_ADD` unchanged)

4.14 adaptations (separate commit): no `linux/xarray.h`, `ida_alloc_max()` /
`ida_free()` replaced by `ida_simple_get()` / `ida_simple_remove()`;
`BINDER_DEBUG_ENTRY()` kept instead of `DEFINE_SHOW_ATTRIBUTE()` in binder.c.

Config: `CONFIG_ANDROID_BINDERFS=y` (with the existing
`CONFIG_ANDROID_BINDER_DEVICES="binder,hwbinder,vndbinder"`). With binderfs
enabled the legacy misc devices are not registered; the devices appear in each
binderfs mount instead.

### Not done
- `features/` directory (`oneway_spam_detection`, `freeze_notification`):
  apollo advertises both, but 4.14 lacks `BINDER_ENABLE_ONEWAY_SPAM_DETECTION`,
  `BINDER_FREEZE`, `BINDER_GET_FROZEN_INFO` and freeze notifications, so
  advertising them would be wrong. They are separate binder.c/binder_alloc.c
  backports (apollo: "BACKPORT: binder: Add freeze notification support for
  Android 14+" and the oneway spam detection series). Neither apollo nor 4.14
  has `BINDER_GET_EXTENDED_ERROR` ("extended_error").
- SELinux policy for `binder` fs type / `/dev/binderfs` labelling is a
  userspace policy matter (genfscon).

### Test on a device
    adb shell mount | grep binder        # init mounts binder at /dev/binderfs
    adb shell ls -l /dev/binderfs        # binder hwbinder vndbinder binder-control features?
    adb shell ls /dev/binderfs/binder_logs   # state stats transactions transaction_log proc/
    adb shell 'mkdir /tmp/b; mount -t binder -o max=4 binder /tmp/b; ls /tmp/b'
Check `dmesg | grep -i binder` for errors, and that servicemanager/hwservicemanager
start (they open /dev/binder, /dev/hwbinder, /dev/vndbinder via the symlinks).
