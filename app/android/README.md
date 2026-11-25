# RollKey Android app (v2, with push alerts)

Full Android Studio project (Kotlin + Jetpack Compose). Package / applicationId `com.rollkey.rollinglock`.
The earlier app (sources only) is kept in `app/archive/android_v1/`.

## What it does
- **Bluetooth code:** scans for the lock (`RollingLock`), bonds with the 6-digit pair key, shows the current rolling code (live updates), copy to clipboard.
- **Lock Wi-Fi setup:** while connected, the *Lock Wi-Fi* card lets the owner put the lock on their Wi-Fi: the lock scans, the app lists the networks (signal bars, 🔒 for secured ones), the owner picks one and types the password, and the lock tests the connection before saving it. No Wi-Fi = no push alerts.
- **Push alerts:** reads the lock's own FCM topic over Bluetooth, subscribes to it and shows notifications on channel `lock_alerts` — even when the app is closed. The topic is saved on the phone, so alerts keep working after the app disconnects. Connecting to a different lock moves the subscription to that lock's topic. Events: test, wrong code, lockout, hard lockout, unlocked, wipe attempted, wipe done. Recent alerts are listed in the app.
- **Wiped lock:** if the lock was wiped, the phone's old pairing no longer works. The app detects this (bonded phone dropped before any code arrives) and shows *Lock was reset – pair again* with a button to Bluetooth settings.
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
│       │   ├── LockBle.kt               ← BLE scan / bond / GATT queue / code, topic, Wi-Fi
│       │   ├── PinStore.kt, PinScreen.kt, Biometric.kt  (from v1)
│       └── res/                  ← icons, theme, notification icon
├── gradle/libs.versions.toml     ← AGP 9.4.1, compileSdk 37, Firebase BoM 34.0.0
└── gradlew / gradlew.bat
```

## Set up push alerts
1. Firebase console → your project → **Add app → Android**, package `com.rollkey.rollinglock`. Download `google-services.json` into `app/android/app/`.
2. Build and install. Connect to the lock once: the app reads the lock's topic and the Alerts card should say **Push alerts on**. Until then it says *Connect to your lock once to turn on alerts*.
3. Set up the lock's Wi-Fi from the *Lock Wi-Fi* card, otherwise the lock cannot send alerts.

The alert topic is no longer built into the app (no build-time secrets file any more): each lock has its own topic and hands it to the app over Bluetooth. Without step 1 the app still builds and the Bluetooth part works; the Alerts card says what is missing.

## Bluetooth protocol
Service `4fafc201-1fb5-459e-8fcc-c5c9c331914b`. The new characteristics (topic, Wi-Fi) need an encrypted, bonded link (the 6-digit passkey), like the code.

| Characteristic | UUID | Access | Value |
|---|---|---|---|
| Code | `beb5483e-36e1-4688-b7f5-ea07361b26a8` | read, notify | current rolling code |
| Topic | `beb5483e-36e1-4688-b7f5-ea07361b26a9` | read | UTF-8 FCM topic, e.g. `rk-` + 32 hex |
| Wi-Fi control | `beb5483e-36e1-4688-b7f5-ea07361b26aa` | write (with response), ≤128 B | `S` = scan; `C` + 1 byte SSID length + SSID + password (rest; empty = open network). SSID ≤32 B, password ≤63 B |
| Wi-Fi status | `beb5483e-36e1-4688-b7f5-ea07361b26ab` | read, notify | first line: `none`, `saved:<ssid>`, `scanning`, `testing`, `ok:<ssid>`, `fail:notfound`, `fail:password`, `fail:timeout`, `fail:busy`, or `scan:done` followed by lines `<rssi>,<locked 0/1>,<ssid>` (strongest first, ≤12) |

After connecting, the app runs these one at a time (Android allows only one GATT operation at once): MTU 247 → enable code notifications → read code → read topic → enable Wi-Fi status notifications → read Wi-Fi status. A `scan:done` notification may be cut to the MTU, so the app then reads the characteristic for the full list.

### Wi-Fi setup flow
*Set up Wi-Fi* → app writes `S` → status `scanning` → `scan:done` + list → user taps a network → password (skipped for open networks) → app writes `C…` → status `testing` → `ok:<ssid>` (saved) or `fail:<reason>` (nothing saved; the app shows *Wrong password*, *Network not found*, … and lets the user try again). A lock wipe keeps the Wi-Fi and the topic.

## Build
```powershell
$env:JAVA_HOME="C:\Program Files\Android\Android Studio\jbr"; .\gradlew.bat assembleDebug
```
APK: `app/build/outputs/apk/debug/app-debug.apk`. Or open the `app/android` folder in Android Studio and press Run.

Installing over the old app keeps the same package, so it replaces it on the phone.
