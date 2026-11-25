# RollKey: Rolling Code Lock

A keypad lock whose access code changes after every use. The **ESP32** lock shows nothing on its screen in standby. The **current code is only available in the RollKey Android app** over a paired Bluetooth link, and the owner gets **push alerts** on their phone for wrong codes, lockouts, unlocks and wipes, from anywhere with internet.

The project started as a pure **AVR Assembly** lock on the ATmega328P (see [Original AVR version](#original-avr-version)) and was rebuilt on the ESP32.

> **Status (Nov 2026):** firmware, app and push alerts are complete and **verified on the real hardware** (full checklist in [firmware/esp32/README.md](firmware/esp32/README.md)).

---

## How it works

```
             keypad + OLED                         Bluetooth LE (paired, encrypted)
  owner ───────────────────────►  ESP32 lock  ◄──────────────────────────────►  RollKey app
                                     │   │                                      (shows code)
                         solenoid ◄──┘   │ Wi-Fi (only when an alert is sent)
                       (IRLZ44N)         ▼
                                 Google OAuth → Firebase Cloud Messaging ──► push alert on the phone
```

1. The lock holds a 16-bit seed. The 4-digit code is derived from it.
2. After each correct entry the seed steps forward (LFSR) and is mixed with keypress timing, so **every code works once**.
3. The new code is sent to the paired phone over Bluetooth. It is **never shown on the lock's screen**.
4. Security events are queued and sent as push alerts by a background task, so the keypad and lock never wait for the network.

---

## Using the lock

| Action | Keys | What happens |
| :--- | :--- | :--- |
| **First power-up** | (prompted) | Set a **wipe PIN** (4 chars, 0-9 and A-D), a **restore number** (4 digits) and a **Bluetooth pair key** (6 digits). Each is typed twice. They are permanent. Only a full flash erase resets them. |
| **Open** | `#` + 4-digit code | `OPEN`, green LED, the lock releases for 3 s. The app receives the next code. |
| **Wrong code** | | `ERROR`, red LED, beep. A second wrong code starts a **deadlock challenge**. |
| **Deadlock challenge** | 2 digits + `#` | The screen shows two digits. Enter the 9's complement of each (e.g. `27` → `72#`). Success does *not* open the lock: you still need `#` + code (the first try after recovery gets one grace pass). |
| **Hard lockout** | | 2 failed challenges → `LOCKED OUT`. Only a correct code or a wipe gets out. |
| **Wipe** | `*` + wipe PIN | The code returns to the restore number, **all paired phones are forgotten**, and the lock restarts and asks for a new pair key. A wrong PIN counts as a wrong code. |
| **Timeout** | | 10 s without a key clears the input. It never counts as a failed attempt. |

The OLED is dark in standby and wakes on `#` or `*`.

---

## Push alerts

| Event | Notification |
| :--- | :--- |
| Wrong code or wrong wipe PIN | Wrong code entered |
| 2 wrong codes (challenge started) | Lock blocked after wrong codes |
| Challenge failed twice | Lock fully locked out |
| Correct code, lock opened | Safe unlocked |
| `*` pressed | Wipe attempted |
| Wipe completed (sent after the restart) | Lock was wiped |

- **Only the event type is sent.** Never a code, PIN, key or token.
- **At most one alert per event type every 30 s.**
- **Wi-Fi is on only while sending,** and never while the solenoid is energised.
- **If Wi-Fi is down,** the lock works normally and that alert is dropped (tested).
- **The lock and the phone don't need the same network.** The lock needs any 2.4 GHz Wi-Fi with internet. The phone receives alerts anywhere.
- **Firebase Spark (free) plan:** the ESP32 signs its own Google OAuth token. No Cloud Functions, no billing.

---

## Android app (RollKey)

- **Code:** scans for `RollingLock`, pairs with the 6-digit key, shows the current code with live updates, and can copy it to the clipboard.
- **Alerts:** subscribes to the lock's alert topic and shows notifications even when the app is closed. Recent alerts are listed in the app.
- **Wiped lock:** detects that the lock forgot this phone and shows *Lock was reset – pair again* with a button to Bluetooth settings (Android only lets Settings remove a pairing).
- **App lock:** 6-digit PIN (salted PBKDF2, slows down after 5 wrong tries), optional fingerprint, auto-lock after 30 s in the background, and screenshots are blocked.

Package `com.rollkey.rollinglock`, Kotlin + Jetpack Compose. Details: [app/android/README.md](app/android/README.md).

---

## Security features

| Feature | ESP32 implementation |
| :--- | :--- |
| Rolling code | 16-bit Galois LFSR (feedback `0xB4`, as in the AVR original), new 4-digit code after every unlock |
| Entropy | keypress timing (`micros()`) mixed into the seed after each unlock; challenge digits from the hardware RNG |
| Persistence | seed, owner numbers and pair key in NVS flash, so a power cut can't bypass the lock |
| Lockout | 2 wrong codes → challenge; 2 failed challenges → hard lockout |
| Gated wipe | owner PIN required; a letter in the PIN can never collide with a numeric code |
| Bluetooth | NimBLE stack, static 6-digit passkey, bonding + MITM + Secure Connections; the code is readable and notified only on an encrypted, authenticated link |
| No code on screen | the OLED never shows the next code |
| Push alerts | FCM HTTP v1 over TLS (certificate-checked); secrets kept out of Git |

---

## Hardware (ESP32 build)

| Component | Part |
| :--- | :--- |
| MCU | ESP32 DevKit (30-pin, WROOM) |
| Display | 0.96″ I2C OLED (SH1106), mounted upside down (`OLED_FLIP 1`) |
| Keypad | 4×4 matrix |
| Lock | solenoid, ~12.6 Ω coil at 9 V (~0.7 A) |
| Driver | IRLZ44N MOSFET: full power 300 ms, then ~40 % PWM hold; 1N4007/1N5822 flyback diode |
| Indicators | red + green LED, passive piezo buzzer (tones near its 2.7 kHz resonance) |
| Power | 5 V 3 A barrel jack charger → UPS module with 2×18650 → 5 V to ESP32 VIN, and 3 A fuse → XL6009 boost → 9 V lock rail; 2× 470 µF 25 V |

| Signal | GPIO |
| :--- | :--- |
| OLED SDA / SCL | 21 / 22 |
| Keypad rows | 13, 14, 27, 26 |
| Keypad columns | 25, 33, 32, 16 |
| Red / green LED | 19 / 23 |
| Buzzer | 18 |
| Solenoid MOSFET gate | 4 |

Schematic, wiring table (wire gauges and currents) and BOM: [hardware/README.md](hardware/README.md).

> **Never power the ESP32 from USB and VIN at the same time.** Disconnect VIN (or switch the adapter off) before plugging in USB.

---

## Getting started

### 1. Firmware
1. Arduino IDE (or `arduino-cli`) with the **ESP32** core 3.3.x.
2. Libraries: **U8g2**, **Keypad**, **NimBLE-Arduino** (2.5.2).
3. Open `firmware/esp32/rolling_code_lock_esp32_alerts/`.
4. Board **ESP32 Dev Module**, Partition Scheme **Huge APP** (the sketch is ~1.4 MB).
5. First time only: *Erase All Flash Before Sketch Upload* = Enabled, so first-time setup runs.

### 2. Firebase and secrets (for push alerts)
1. In the Firebase console, add an Android app `com.rollkey.rollinglock` and save `google-services.json` in `app/android/app/`.
2. In Google Cloud, create a service account with only the **Firebase Cloud Messaging API Admin** role and save its JSON key in `secrets/`.
3. Run:
   ```powershell
   python firmware/esp32/tools/make_secrets.py secrets/fcm-key.json
   ```
   It asks for the Wi-Fi name and password and writes `secrets.h` (firmware) and `secrets.properties` (app) with a shared random topic.

Without these files everything still builds; alerts are simply off.

### 3. App
Open `app/android` in Android Studio and press Run, or build an APK:
```powershell
$env:JAVA_HOME="C:\Program Files\Android\Android Studio\jbr"; .\gradlew.bat assembleDebug
```

### Build settings
| Setting | File | Final | Bench testing |
| :--- | :--- | :--- | :--- |
| `DEBUG` | sketch `.ino` | 0 | 1 (prints codes to Serial) |
| `ALERT_TEST_ON_BOOT` | `alerts.h` | 0 | 1 (test alert at every boot) |
| `ALERTS_LOG` | `alerts.h` | 0 | 1 (alert progress + memory) |

---

## Repository structure

```
rollinglock/
├── firmware/
│   ├── esp32/
│   │   ├── rolling_code_lock_esp32_alerts/   ← MAIN: lock + push alerts (alerts.h/.cpp, secrets.example.h)
│   │   ├── rolling_code_lock_esp32_oled_final/ ← lock without alerts (Bluedroid BLE)
│   │   ├── variants/                         ← 1 s solenoid, keypad remap, preset-PIN test builds
│   │   ├── tests/                            ← OLED, keypad, solenoid, all-components, power tests
│   │   ├── tools/make_secrets.py             ← writes the git-ignored secret files
│   │   └── archive/                          ← older LCD-era sketch
│   └── avr/                                  ← original ATmega328P version (docs only)
├── app/
│   ├── android/                              ← RollKey app (Android Studio project)
│   └── archive/android_v1/                   ← earlier app sources
├── hardware/                                 ← schematics (+ revisions), wiring table, photos
├── docs/                                     ← AVR status/roadmap, state-machine flowchart
├── secrets/                                  ← service account key (git-ignored, local only)
├── HANDOFF.md                                ← push-alert design brief and status
└── README.md
```

Git-ignored and local only: `secrets/`, `secrets.h`, `secrets.properties`, `google-services.json`, `local.properties`, and build output.

---

## Original AVR version

- **MCU:** ATmega328P @ 16 MHz, pure AVR Assembly, < 1.1 KB flash.
- **Interface:** 4-bit parallel LCD + 4×3 keypad, solenoid via transistor, buzzer, red/green LEDs.
- **Same core idea:** LFSR rolling code, `TCNT0` keypress entropy, EEPROM persistence, 9's-complement deadlock, `*` factory wipe.

See [docs/current_status.md](docs/current_status.md) and [docs/system_flowchart.md](docs/system_flowchart.md).

---

## Team

Built by a team of four (EE 356 course project, University of Peradeniya).

## License

MIT
