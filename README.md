# RollKey — Rolling Code Lock

A professional-grade security system built in two stages: an original **AVR Assembly** implementation on the ATmega328P, then an upgraded **ESP32** version with Bluetooth (BLE), and an Android companion app with push alerts over Firebase.

---

## Repository Structure

```
rollinglock/
├── firmware/
│   ├── avr/                                   ← ATmega328P Assembly (original; source not yet added)
│   └── esp32/
│       ├── rolling_code_lock_esp32_alerts/     ← MAIN sketch: lock + push alerts
│       ├── rolling_code_lock_esp32_oled_final/ ← lock without alerts
│       ├── tools/make_secrets.py              ← writes the git-ignored secret files
│       ├── variants/                          ← 1 s solenoid, keypad-remap, preset-PIN builds
│       ├── tests/                             ← OLED/keypad and power bring-up tests
│       └── archive/                           ← older LCD-era BLE sketch
├── app/
│   ├── android/                               ← Android Studio project v2: BLE code + push alerts
│   └── archive/android_v1/                    ← earlier app sources
├── hardware/
│   ├── schematics/                            ← current schematic (+ archive/ of revisions)
│   ├── wiring/                                ← wiring connection table (.docx)
│   ├── photos/                                ← annotated module photos
│   └── pcb/ enclosure/ bom/                   ← placeholders
├── docs/                                      ← AVR status/roadmap and state-machine flowchart
└── README.md
```

---

## System Overview

### AVR Version (Original)
- **MCU:** ATmega328P @ 16 MHz
- **Language:** Pure AVR Assembly
- **Interface:** 4-bit parallel LCD + 4×3 matrix keypad
- **Code size:** < 1.1 KB Flash

### ESP32 Version (Current)
- **MCU:** ESP32
- **Interface:** 1.3″ I2C OLED + 4×4 matrix keypad
- **Connectivity:** BLE (bonded phones read the current code). Firebase push alerts (ESP32 → FCM HTTP v1 → app), see `rolling_code_lock_esp32_alerts`
- **Power:** UPS module (5 V / 18650) + XL6009E1 boost → 9 V solenoid rail

---

## Core Security Features

| Feature | Description |
|---|---|
| **Rolling Code (LFSR)** | 16-bit LFSR with CCITT-16 polynomial; unique 4-digit code every unlock |
| **Entropy Injection** | Samples hardware timer `TCNT0` on keypresses for true randomness |
| **EEPROM Persistence** | Seed and state survive power cuts |
| **Deadlock / Challenge-Response** | 2 failed attempts → 9's-complement recovery challenge |
| **Factory Wipe** | Clears EEPROM + paired BLE devices; prompts new BT passkey on next boot |
| **BLE Passkey Pairing** | Secure pairing before the app can receive the next code |
| **Owner Wipe PIN** | Wipe is gated by a 4-char owner PIN; restores a fixed restore number |
| **Push Alerts** | Wrong code, lockout, unlock and wipe events pushed to the owner's phone via Firebase |

---

## Hardware Stack (ESP32 Build)

| Component | Detail |
|---|---|
| MCU | ESP32 |
| Display | 1.3″ I2C OLED |
| Keypad | 4×4 matrix |
| Lock | Small solenoid (~12.6 Ω coil, 9 V) |
| Driver | IRLZ44N MOSFET (pull-in then PWM hold) |
| Power backup | 5 V UPS module (Type-C in, 18650 cell) + XL6009E1 boost → 9 V |

---

## Android App

- **Package:** `com.rollkey.rollinglock`
- **Language:** Kotlin + Jetpack Compose (native Android)
- **Features:** BLE connection & bonding, live code display (notify), push alerts (FCM topic), app PIN (PBKDF2) + fingerprint unlock

---

## Team

Built by a team of four (EE 356 course project, University of Peradeniya).

---

## License

MIT
