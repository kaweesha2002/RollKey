# Android Companion App

Native Android app (**Kotlin + Jetpack Compose**) for the RollKey rolling code lock.
Package: `com.rollkey.rollinglock`

## What it does
- Scans for and connects to the lock (`RollingLock`) over BLE, bonds using the lock's 6-digit pair key.
- Reads / receives (notify) the current rolling code and shows it with an "updated at" time; copy to clipboard.
- App lock: 6-digit app PIN stored as a salted PBKDF2 hash, with slow-down after 5 wrong tries.
- Optional fingerprint / strong-biometric unlock of the app.

## Files

```
android/
├── app/src/main/java/com/rollkey/rollinglock/
│   ├── MainActivity.kt   ← UI + flow (FragmentActivity, fingerprint-enabled version)
│   ├── LockBle.kt        ← BLE scan / connect / bond / read+notify, LockUiState
│   ├── PinScreen.kt      ← PIN pad UI (create / unlock), biometric button
│   ├── PinStore.kt       ← PBKDF2 PIN storage, fail counter, biometric flag
│   └── Biometric.kt      ← BiometricPrompt helper (BIOMETRIC_STRONG)
├── AndroidManifest_snippet.xml ← BLE permissions to paste into AndroidManifest.xml
├── assets/icon/          ← launcher icon (SVG foreground, 1024 px foreground, 512 px full)
└── archive/MainActivity_no_fingerprint.kt.txt ← earlier version without biometrics
```

Only the source files are kept here, not a full Gradle project.

## Setting up the project
1. Android Studio → *New Project → Empty Activity (Compose)*, package `com.rollkey.rollinglock`.
2. Copy the `.kt` files into `app/src/main/java/com/rollkey/rollinglock/`.
3. Paste `AndroidManifest_snippet.xml` contents inside `<manifest>` above `<application>`.
4. Add dependencies: `androidx.biometric:biometric` and `androidx.fragment:fragment-ktx`.
5. Import the icon via *New → Image Asset* using `assets/icon/`.
6. Build an APK (*Build → Build APK(s)*) and share it directly.

To build without fingerprint support, use `archive/MainActivity_no_fingerprint.kt.txt`
as `MainActivity.kt` and drop `Biometric.kt`.
