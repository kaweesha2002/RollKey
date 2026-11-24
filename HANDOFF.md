# Handoff: add push alerts to the rolling code lock (ESP32 sends FCM directly)

> **Status (2026-11-24):** built and verified end to end. Firmware: `firmware/esp32/rolling_code_lock_esp32_alerts/` (based on `oled_final`, BLE moved to NimBLE for memory). App: `app/android/` (new project v2). A test alert reached the phone. All event alerts verified on adapter power; final firmware (test alert, DEBUG and logs off) flashed. Wi-Fi-down test passed (lock unaffected). Full checklist complete — see `firmware/esp32/README.md`.

Read this file first. The Firebase-side checklist (Stages 2 to 4) is **not in this repository yet**: the root `README.md` is the project overview. Ask me for the checklist before the Android step.

## Who and what

I am a student building a rolling-code digital lock (EE 356 product design). The firmware runs on an ESP32 and there is an Android app. I want a push notification on my phone when someone tries the lock, even when I am away.

The Firebase project already exists on the **Spark (free) plan**. **Do not upgrade it, do not enable billing, and do not use Cloud Functions.** The ESP32 itself sends the FCM message.

## Current state of the project

| Part | State |
| :--- | :--- |
| Firmware | `firmware/esp32/variants/rolling_code_lock_esp32_oled_1s_preset/rolling_code_lock_esp32_oled_1s_preset.ino`. Works on the bench. |
| Board | ESP32 DevKit (30-pin, WROOM), on **COM3** (CP210x). FQBN `esp32:esp32:esp32doit-devkit-v1`, ESP32 core 3.3.12. Partition scheme **Huge APP** (the sketch is ~1.15 MB with BLE alone). |
| Build tools | `arduino-cli` bundled with the Arduino IDE: `%LOCALAPPDATA%\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe`. Pass `--libraries "%USERPROFILE%\Documents\Arduino\libraries"` or it will not find U8g2. Close the IDE's Serial Monitor before uploading (COM3 busy otherwise). |
| Libraries | U8g2 (OLED), Keypad. Bluetooth and Preferences come with the core. |
| Android app | Kotlin and Compose. Pairs over BLE and shows the rolling code. Builds and runs on a Samsung SM-A307FN. This repo holds only the `.kt` sources (`app/android/app/src/main/java/com/rollkey/rollinglock/`); the full Android Studio project (with `app/build.gradle.kts` and the `applicationId`) lives elsewhere — ask me for its path. |
| Firebase | Project created (Spark). Android app registration, `google-services.json` and the Android changes are still to do. |
| Phone | Samsung, Google Play services present |

### Firmware facts you must respect

- **Bluetooth is the channel for the rolling code and must keep working.** BLE service `RollingLock`, always advertising (`BLE_ALWAYS_ON 1`), static-passkey pairing with a 6-digit key typed on the keypad.
- **Pins:** OLED I2C SDA 21, SCL 22. Keypad rows 13, 14, 27, 26 and columns 25, 33, 32, 16. **Red LED 19, green LED 23** (swapped to match the wiring), buzzer 18, solenoid gate 4 (IRLZ44N).
- **OLED** is mounted upside down: `OLED_FLIP 1` rotates it 180°.
- **Buzzer** is a passive piezo, loudest at ~2700 Hz. All beeps use 1800–3200 Hz.
- **Keypad remap:** the keypad's last row is dead, so `KEY_REMAP 1` makes A act as `#`, B as `*`, and C as `0`.
- **Solenoid:** plain on/off for `SOLENOID_OPEN_MS` (1000 ms). No PWM.
- **Testing settings:** `DEBUG 1` prints the current code to Serial, and `PRESET_OWNER 1` stores wipe PIN 1234 and restore number 4321. Both are for testing only.
- **Power:** a Type-C 5V charger feeds a UPS module with 18650 cells, whose 5V output feeds the ESP32 VIN and a boost to 9V for the lock. Wi-Fi current spikes must not coincide with a solenoid pulse.
- The Arduino IDE needs the sketch in a folder with the same name as the `.ino`.

## The feature

Events to alert on: wrong code, lockout, safe unlocked, wipe attempted, wipe done.

**Wipe done:** `systemWipe()` calls `ESP.restart()` about 7 s after the wipe, so an alert queued there would be lost. Save a "wipe done" flag in NVS instead, and send the alert after the reboot (then clear the flag).

**Rules**
1. **Never put a code, PIN, pair key or any secret in a message or a log.** Send only the event type.
2. **Never block the keypad, the OLED or the lock.** Send from a separate FreeRTOS task fed by a queue.
3. Turn Wi-Fi on only when there is an alert to send, and off afterwards.
4. Do not transmit while the solenoid is energised (a `lockBusy` flag).
5. Rate limit to one alert per event type every 30 seconds.
6. Do not change the lock logic, the rolling code, the enrolment or the wipe behaviour. Hook alerts in with one-line calls only, and show me each place before editing it.

## Design: ESP32 sends the FCM message itself

```
ESP32 --(NTP time)--> sign JWT with a service account key --> OAuth token
      --> HTTPS POST to FCM HTTP v1 --> topic "lock-alerts-<random>" --> Android app
```

- Use the **FCM HTTP v1 API**. The legacy API is gone.
- Service account: restricted to the **Firebase Cloud Messaging API Admin** role only (I will create it and the key myself).
- The access token lasts about an hour. Cache it and reuse it, instead of signing every alert.
- Send a **notification message** to the topic, with Android channel `lock_alerts`, so it is shown even when the app is closed.

## The hard part: memory

Wi-Fi, Bluetooth and TLS together may not fit in RAM. Measure before building.

1. Add temporary logging of `ESP.getFreeHeap()` and `ESP.getMinFreeHeap()`: at boot, with BLE running, with a phone connected, and during a TLS connection.
2. If the lowest free heap during TLS is below about 30 KB, or anything crashes, try in this order: pause BLE advertising while sending, switch to the NimBLE-Arduino stack, trim task stack sizes, and as a last resort send alerts only when no phone is connected.
3. Choose the Huge APP partition scheme if the sketch is too big.
4. **Fallback:** keep the sender behind one function, `sendAlert(event)`, so it can be swapped for an ntfy sender (a plain HTTPS POST of the event text to `https://ntfy.sh/<random-topic>`) if FCM does not fit.

## Secrets

- The service account key, the Wi-Fi password and the topic name live in a git-ignored `secrets.h` in the sketch folder, and a git-ignored `secrets/` folder.
- Never print, log or commit them. Never paste them into chat.
- Create a `.gitignore` for them before any commit.

## Android side (after the firmware design is settled)

Follow Stages 2 to 4 of `README.md`: Gradle plugin and BoM, manifest permission and channel metadata, `MainActivity` creating the `lock_alerts` channel, asking for `POST_NOTIFICATIONS` on Android 13+, and subscribing to the topic. Build the project to check it compiles.

## Order of work, with stops

1. Read the sketch and summarise how it is structured. **Stop and show me.**
2. Add the heap logging and tell me exactly what to flash and what to read. **Stop.**
3. Design the alert module (own tab or files), and show me the plan. **Stop.**
4. Implement it, compile it (with `arduino-cli` if installed, otherwise tell me to compile in the Arduino IDE), and give me a test with one hard-coded alert. **Stop.**
5. After my hardware test passes, hook the events in, one at a time.
6. Do the Android changes and build them.
7. Write up the final settings and a short test checklist.

## Ask me before

- Anything involving login, billing, deploying or deleting files.
- Editing outside the project folder, or adding a dependency.
- Changing any firmware behaviour other than adding the alert calls.

## What you cannot do, so I will

Check the notification on my phone, watch the lock and screen, and create the Google Cloud service account and its key.

Claude **can** compile, flash the ESP32 over COM3 and read its serial output when the board is plugged into this PC — but ask me before each flash.
