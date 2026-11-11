# Shared by watch_build.sh and watch_defconfig.sh: the one place that knows
# which fragments make up a device config. Source it, do not run it.
# Needs: usage() defined by the caller; SRC; for watch_assemble/watch_verify
# also MK() and O set. Call them plainly (not as `f || x`), so the caller's
# `set -e` stays active inside them; they fail through errexit/return 1.

# watch_select DEVICE: sets DEVFRAGS, FRAGS, CFG, DEFCONFIG (file name without
# path). dw100u is accepted as the name of the shared silver/v13m config.
watch_select() {
  case $1 in
    dm63)                      DEVFRAGS="dm63_touch watch_hrs watch_sc7a22 dm63_aod dm63_sensors gpu_mali_sharkle" ;;
    dw100u-silver|dw100u-v13m|dw100u) DEVFRAGS="dw100u_touch watch_hrs watch_sc7a22 dw100u_aod dw100u_fp dw100u_sensors gpu_mali_sharkle" ;;
    mt107)                     DEVFRAGS="mt107 gpu_mali_sharkle" ;;
    mt108)                     DEVFRAGS="mt108 gpu_mali_sharkle" ;;
    dm82)                      DEVFRAGS="dm82 gpu_mali_sharkle" ;;
    k18)                       DEVFRAGS="k18 gpu_mali_sharkle" ;;
    *) usage ;;
  esac
  # parity before hardening: hardening wins on any overlap
  FRAGS="watch_base watch_stock_parity watch_hardening $DEVFRAGS"
  CFG=$SRC/arch/arm64/configs
  case $1 in dw100u-*) DEFCONFIG=dw100u_defconfig ;; *) DEFCONFIG=$1_defconfig ;; esac
}

# watch_assemble: sprd_sharkle_defconfig + fragments + olddefconfig into $O/.config
watch_assemble() {
  local f files
  cd "$SRC"
  echo "== config: sprd_sharkle_defconfig + $FRAGS"
  MK sprd_sharkle_defconfig
  files=()
  for f in $FRAGS; do files+=("$CFG/$f.fragment"); done
  # cwd = $O: merge_config.sh drops its ./.tmp.config.* temp file in cwd
  (cd "$O" && "$SRC/scripts/kconfig/merge_config.sh" -m -O "$O" "$O/.config" "${files[@]}" >/dev/null)
  MK olddefconfig
}

# watch_verify: fail unless every fragment line took effect in $O/.config
watch_verify() {
  local f g l line opt last bad=0
  echo "== verify fragments"
  for f in $FRAGS; do
    [ -r "$CFG/$f.fragment" ] || { echo "FAIL: cannot read $CFG/$f.fragment" >&2; return 1; }
    while IFS= read -r line; do
      case $line in
        CONFIG_*=*) ;;
        "# CONFIG_"*" is not set") ;;
        *) continue ;;
      esac
      opt=${line#\# }; opt=${opt%%[= ]*}
      # a later fragment may override an earlier one: check only the last setting
      last=
      for g in $FRAGS; do
        l=$(grep -E "^(# )?${opt}([= ]|$)" "$CFG/$g.fragment" | tail -n1 || true)
        if [ -n "$l" ]; then last=$l; fi
      done
      [ "$line" = "$last" ] || continue
      if ! grep -qxF "$line" "$O/.config"; then
        # a symbol olddefconfig does not write (unreachable) counts as "not set"
        case $line in
          "# CONFIG_"*" is not set") grep -qE "^${opt}=" "$O/.config" || continue ;;
        esac
        echo "MISMATCH ($f): wanted \"$line\", got \"$(grep -E "^(# )?${opt}([= ]|$)" "$O/.config" || echo missing)\"" >&2
        bad=$((bad+1))
      fi
    done < "$CFG/$f.fragment"
  done
  if [ "$bad" -ne 0 ]; then echo "FAIL: $bad fragment option(s) did not take effect" >&2; return 1; fi
}
