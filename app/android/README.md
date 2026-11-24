# RollKey Android app (v2, with push alerts)

Full Android Studio project (Kotlin + Jetpack Compose). Package / applicationId `com.rollkey.rollinglock`.
The earlier app (sources only) is kept in `app/archive/android_v1/`.

## What it does
- **Bluetooth code:** scans for the lock (`RollingLock`), bonds with the 6-digit pair key, shows the current rolling code (live updates), copy to clipboard.
- **Push alerts:** subscribes to the lock's FCM topic and shows notifications on channel `lock_alerts` — even when the app is closed. Events: test, wrong code, lockout, hard lockout, unlocked, wipe attempted, wipe done. Recent alerts are listed in the app.
- **App lock:** 6-digit PIN (salted PBKDF2), optional fingerprint, auto-lock after 30 s in the background, screenshots blocked.

## Files
```
android/
├── app/
│   ├── build.gradle.kts          ← Firebase plugin applied only if google-services.json exists
│   ├── google-services.json      ← YOU add this (git-ignored)
│   └── src/main/
│       ├── AndroidManifest.xml   ← BLE + notification permissions, FCM service, channel metadata
│       ├── java/com/rollkey/rollinglock/
│       │   ├── MainActivity.kt          ← screens, permission requests, Alerts card
│       │   ├── Alerts.kt                ← channel, topic subscription, alert history
│       │   ├── LockMessagingService.kt  ← receives FCM messages
│       │   ├── LockBle.kt               ← BLE scan / bond / notify (from v1)
│       │   ├── PinStore.kt, PinScreen.kt, Biometric.kt  (from v1)
│       └── res/                  ← icons, theme, notification icon
├── secrets.example.properties    ← copy to secrets.properties (git-ignored): ALERT_TOPIC
├── gradle/libs.versions.toml     ← AGP 9.4.1, compileSdk 37, Firebase BoM 34.0.0
└── gradlew / gradlew.bat
```

## Set up push alerts
1. Firebase console → your project → **Add app → Android**, package `com.rollkey.rollinglock`. Download `google-services.json` into `app/android/app/`.
2. Create `secrets.properties` (or run `python firmware/esp32/tools/make_secrets.py <key.json>`, which writes it together with the ESP32's `secrets.h`). `ALERT_TOPIC` must equal `FCM_TOPIC` on the ESP32.
3. Build and install. The Alerts card should say **Push alerts on**.

Without steps 1–2 the app still builds and the Bluetooth part works; the Alerts card says what is missing.

## Build
```powershell
$env:JAVA_HOME="C:\Program Files\Android\Android Studio\jbr"; .\gradlew.bat assembleDebug
```
APK: `app/build/outputs/apk/debug/app-debug.apk`. Or open the `app/android` folder in Android Studio and press Run.

Installing over the old app keeps the same package, so it replaces it on the phone.
