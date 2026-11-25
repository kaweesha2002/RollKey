# Firmware — ESP32

Arduino-framework sketches for the ESP32 version of the rolling code lock.
Each sketch lives in a folder with the same name (the Arduino IDE requires this).

## Which sketch to flash

| Folder | Purpose |
| :--- | :--- |
| `rolling_code_lock_esp32_alerts/` | **Main build + push alerts.** Copy of `oled_final` plus `alerts.h/.cpp`: Wi-Fi → FCM HTTP v1 topic message on wrong code, lockout, hard lockout, unlock, wipe attempted/done. **Wi-Fi is set up from the app over Bluetooth**, and each lock has its own alert topic. Needs `secrets.h` (git-ignored; see `secrets.example.h` or `tools/make_secrets.py`). Without it, builds with alerts off. Uses the **NimBLE-Arduino** library (not the core's Bluedroid BLE) to leave RAM for HTTPS. **Board: ESP32 Dev Module, Partition: Huge APP** (1.4 MB). |
| `rolling_code_lock_esp32_oled_final/` | **Main build.** OLED + 4×4 keypad + BLE. Solenoid uses PWM pull-in (300 ms) then ~40 % hold, open for 3 s. |
| `variants/rolling_code_lock_esp32_oled_1s/` | Same logic, plain on/off solenoid for 1 s (no PWM). |
| `variants/rolling_code_lock_esp32_oled_1s_remap/` | 1 s build + `KEY_REMAP 1`: the keypad's bottom row (`* 0 # D`) was dead, so `A = #`, `B = *`, `C = 0`. Wipe PIN becomes digits only. |
| `variants/rolling_code_lock_esp32_oled_1s_preset/` | Remap build + `PRESET_OWNER 1`: stores wipe PIN `1234` / restore number `4321` on first boot. **Testing only — never ship.** |
| `archive/rolling_code_lock_esp32_ble/` | Earlier "Stage 7" build written for the 16×2 LCD (3 s solenoid). Kept for reference. |
| `tests/oled_keypad_test/` | Bring-up test: guided press-every-key test for the keypad, shown on the OLED + Serial (115200). |
| `tests/keypad_led_test/` | Same keypad/LED pins as the lock: red → green LED check at boot, then each key shown on the OLED (green flash for digits, red for letters, `*`, `#`); hold `#` 2 s to repeat the LED check. |
| `tests/solenoid_test/` | Fires the lock from the BOOT button or Serial (`f`, `p`, `a`). |
| `tests/all_components_test/` | Loops OLED, red/green LEDs, buzzer modes and lock; `s` on Serial skips the lock. |
| `tests/oled_power_test/` | Bring-up test on battery/5 V power: shows reset reason and boot count to catch brownouts; can optionally pulse the lock. |

## Pin map (all current sketches)

| Peripheral | GPIO |
| :--- | :--- |
| OLED (I2C, SH1106 1.3″) | SDA 21, SCL 22 (powered from 3V3) |
| Keypad 4×4 rows 1-4 | 16, 32, 33, 25 (connector plugged in reversed) |
| Keypad 4×4 columns 1-4 | 26, 27, 14, 13 |
| Red LED / Green LED | 23 / 19 (220 Ω each) |
| Buzzer | 18 |
| Solenoid MOSFET gate | 4 → 220 Ω → IRLZ44N (10 k gate pull-down) |

## Behaviour (summary)

- **First-boot enrollment (permanent):** 4-char *wipe PIN* (0-9, A-D) and 4-digit *restore number*, stored in the `owner` NVS namespace. No change path — to start over, upload with *Tools → Erase All Flash Before Sketch Upload*.
- **Rolling code:** 16-bit LFSR (tap `0xB4`), 4-digit numeric code, new code after every unlock, state in NVS.
- **Entry:** `#` + code. 10 s inter-key timeout (does not count as a failure).
- **Deadlock:** 2 wrong codes → challenge (hardware-RNG digits, answer with 9's complement); 2 failed recoveries → hard lockout. Recovery does *not* open the lock; the first code attempt afterwards gets a grace pass.
- **Wipe:** `*` + wipe PIN → code returns to the restore number, all bonded phones and the BLE pair key are erased, device restarts and asks for a new 6-digit pair key.
- **Display:** OLED sleeps in standby, wakes on `#` / `*`. The code is never shown on the OLED.
- **BLE:** advertises as `RollingLock`; current code is on a READ+NOTIFY characteristic, readable only by phones bonded with the 6-digit pair key. `BLE_ALWAYS_ON 1` by default (else 60 s window after unlock).
  - Service UUID `4fafc201-1fb5-459e-8fcc-c5c9c331914b`
  - Characteristics (all need an encrypted, passkey-paired link):

    | UUID | Name | Access | Content |
    | :--- | :--- | :--- | :--- |
    | `beb5483e-…-ea07361b26a8` | code | read + notify | current 4-digit code |
    | `beb5483e-…-ea07361b26a9` | topic | read | this lock's alert topic (`rk-` + 32 hex, made on first boot) |
    | `beb5483e-…-ea07361b26aa` | Wi-Fi control | write | `S` = scan; `C` + 1-byte SSID length + SSID + password = test and save |
    | `beb5483e-…-ea07361b26ab` | Wi-Fi status | read + notify | `none`, `saved:<ssid>`, `scanning`, `scan:done` (+ lines `rssi,locked,ssid`), `testing`, `ok:<ssid>`, `fail:notfound\|password\|timeout\|busy` |
- **Wi-Fi setup:** the lock scans (2.4 GHz only), the app sends the chosen network, and the lock **tests it before saving**. A failed test keeps the old network. Scan, test and alerts all run in one background task, never during the solenoid pulse.
- **Saved network settings:** NVS namespace `net` (`ssid`, `pass`, `topic`). A wipe keeps them; *Erase All Flash* clears them (full factory reset for resale).

## Building

1. Arduino IDE with the **ESP32** board package; board *ESP32 Dev Module*.
2. Install **U8g2** (OLED) and **Keypad** libraries. BLE is built into the ESP32 core; the alerts sketch also needs **NimBLE-Arduino** (2.5.2).
3. If the sketch is too large: *Tools → Partition Scheme → Huge APP*.
4. Set `DEBUG 0` for the final build.

> Push alerts exist only in `rolling_code_lock_esp32_alerts/` (see its `alerts.h`).
> The header comment in the lock sketches still says "12 V bus … buck converter"; the
> actual power design is 5 V UPS → XL6009 boost → 9 V (see `hardware/`).

## Push alerts (`rolling_code_lock_esp32_alerts`)

Verified on the bench: all event alerts delivered to the phone on adapter power (2026-11-24).

| Measurement | Value |
| :--- | :--- |
| Free heap after BLE (NimBLE) | ~107 KB (Bluedroid left only 49 KB and HTTPS failed) |
| Lowest free heap during HTTPS | ~43 KB |
| Alert task stack | 8 KB, ~3.7 KB spare at peak |
| Wi-Fi connect | ~1.2–1.6 s |

### Settings before final use
The repository now holds the **final** values (all 0 below except the rate limit). Set them back to 1 for bench testing with the Serial Monitor.

| Setting | File | Bench | Final |
| :--- | :--- | :--- | :--- |
| `ALERT_TEST_ON_BOOT` | `alerts.h` | 1 | **0** (else a test alert on every power-up) |
| `DEBUG` | `.ino` | 1 | **0** (DEBUG prints the current code to Serial) |
| `ALERTS_LOG` | `alerts.h` | 1 | **0** (Serial is not started when `DEBUG` is 0) |
| `ALERT_RATE_LIMIT_MS` | `alerts.h` | 30000 | 30000 |
| `PROVISION_FROM_SECRETS` | `alerts.h` | 1 (copy `WIFI_SSID`/`WIFI_PASS` from `secrets.h` if no Wi-Fi saved) | **0** for locks you sell (Wi-Fi only from the app) |

Secrets: run `python firmware/esp32/tools/make_secrets.py secrets/fcm-key.json` (writes the git-ignored `secrets.h` with the service account). Add `--wifi` to pre-load Wi-Fi on a bench lock. Wi-Fi and the topic are otherwise set per lock from the app.

After switching BLE stacks, phones must **unpair RollingLock** in Android Bluetooth settings and pair again (6-digit key).

### Test checklist (keypad + lock connected)
- [x] Test alert on boot reaches the phone (app closed)
- [x] `#` + wrong code → "Wrong code entered"
- [x] Second wrong code → "Lock blocked after wrong codes"
- [x] Two failed recovery answers → "Lock fully locked out"
- [x] `#` + correct code → lock opens, then "Safe unlocked" (Wi-Fi only after the solenoid releases)
- [x] `*` → "Wipe attempted"; correct wipe PIN → after reboot, "Lock was wiped"
- [x] Same event twice within 30 s → only one alert
- [x] Wi-Fi off / router unplugged → lock still works normally, alert just fails (not resent later)
- [x] App: pair with the 6-digit key, code shows and updates after an unlock
- [x] Power from the adapter (no USB): no resets during unlock + alert (check reset reason)

### Wi-Fi setup from the app (tested 2026-11-25)
- [x] Existing lock upgraded without erase: Wi-Fi and topic copied from `secrets.h` into NVS, app reads the topic (Alerts: *Push alerts on*)
- [x] Scan lists the networks the lock sees (19–20 found)
- [x] Wrong password → *Wrong password* (handshake rejected after ~6 s), nothing saved
- [x] Correct password → *Connected* (1.2–3.8 s), saved
- [x] Unlock alert delivered over the newly set network (phone hotspot)
- [x] Lowest free heap during Wi-Fi jobs ~93 KB
- [ ] App refreshes Android's Bluetooth cache by itself after a firmware update (needs the next firmware update to test; workaround: unpair + pair again)
- [ ] Buyer flow: `PROVISION_FROM_SECRETS 0` + Erase All Flash → first setup → Wi-Fi only from the app → new random topic
