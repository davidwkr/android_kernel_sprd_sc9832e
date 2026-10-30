# Build the overlay APK (not committed as a binary)

Needs Android SDK build-tools (aapt, zipalign, apksigner) and platform android.jar API 27.

    aapt package -f -M AndroidManifest.xml -S res -I $ANDROID_SDK/platforms/android-27/android.jar -F aodsleep-unsigned.apk
    zipalign -f 4 aodsleep-unsigned.apk aodsleep-aligned.apk
    apksigner sign --ks <your debug/test keystore> --out AodSleepOverlay.apk aodsleep-aligned.apk

Install as /vendor/overlay/AodSleepOverlay.apk (Magisk: system/vendor/overlay/).
Static overlays are only read at boot; reboot after changing it. Verify:
`cmd overlay list` shows `[x] org.dw100u.overlay.aodsleep` under android.
