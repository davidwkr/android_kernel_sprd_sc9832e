# Build the overlay APK (not committed as a binary)

Needs aapt, zipalign, apksigner. `isStatic` and `priority` are @hide in API 27, so the SDK android.jar
fails ("attribute isStatic not found"). Link against the device's own framework instead:
`adb pull /system/framework/framework-res.apk` (read-only; device owner's OK) or take it from the build output, and pass it to `-I`.

    aapt package -f -M AndroidManifest.xml -S res -I framework-res.apk -F aodsleep-unsigned.apk
    zipalign -f 4 aodsleep-unsigned.apk aodsleep-aligned.apk
    apksigner sign --ks <your debug/test keystore> --out AodSleepOverlay.apk aodsleep-aligned.apk

Install as /vendor/overlay/AodSleepOverlay.apk (Magisk: system/vendor/overlay/).
Static overlays are only read at boot; reboot after changing it. Verify:
`cmd overlay list` shows `[x] org.dw100u.overlay.aodsleep` under android.
