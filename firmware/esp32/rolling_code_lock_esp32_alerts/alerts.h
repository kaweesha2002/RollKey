// Push alerts + Wi-Fi setup for the rolling code lock.
//
// The lock calls sendAlert(event) with one line at each place an event
// happens. sendAlert() never blocks: it only rate-limits and drops the event
// into a queue. A separate FreeRTOS task (on core 0) does all Wi-Fi work, one
// job at a time:
//   alert : Wi-Fi on -> NTP time -> OAuth token (signed JWT, cached ~1 h)
//           -> HTTPS POST to FCM HTTP v1 (topic message) -> Wi-Fi off
//   scan  : list the 2.4 GHz networks the lock can see (for the app)
//   test  : try new Wi-Fi details; save them only if the connection works
//
// Wi-Fi name/password and the alert topic are stored in NVS (namespace "net"),
// set by the owner from the app over Bluetooth. Each lock makes its own random
// topic on first boot. A wipe keeps them; "Erase All Flash" clears them.
//
// Only the event type is ever sent or logged: never a code, PIN, pair key,
// Wi-Fi password, token or any other secret.
//
// secrets.h (git-ignored) holds the Google service account. Copy
// secrets.example.h to secrets.h and fill it in, or run
// firmware/esp32/tools/make_secrets.py. Without secrets.h the sketch still
// builds; Wi-Fi setup works but no alerts are sent.
#pragma once
#include <Arduino.h>

// ---------------- Settings ----------------
#define ALERTS_LOG          0        // 1 = print alert progress + heap numbers to Serial
#define ALERT_TEST_ON_BOOT  0        // 1 = send one "test" alert after every boot (bench test)
#define ALERT_RATE_LIMIT_MS 30000UL  // at most one alert per event type every 30 s
#define ALERT_WIFI_TIMEOUT_MS 15000UL
#define ALERT_TASK_STACK    8192     // bytes; measured peak ~4.5 KB during a full HTTPS send
// 1 = on first boot, copy WIFI_SSID / WIFI_PASS from secrets.h into NVS if no
//     Wi-Fi is saved yet (developer convenience). 0 = Wi-Fi only from the app
//     (what a buyer gets). The topic is always taken from FCM_TOPIC in
//     secrets.h if defined and none is saved, otherwise generated randomly.
#define PROVISION_FROM_SECRETS 1

enum AlertEvent : uint8_t {
  ALERT_TEST = 0,
  ALERT_WRONG_CODE,      // wrong rolling code or wrong wipe PIN
  ALERT_LOCKOUT,         // 2 wrong codes: deadlock challenge started
  ALERT_HARD_LOCKOUT,    // deadlock recovery exhausted
  ALERT_UNLOCKED,        // correct code, solenoid fired
  ALERT_WIPE_ATTEMPT,    // '*' pressed: wipe PIN being asked for
  ALERT_WIPE_DONE,       // wipe accepted (sent after the reboot)
  ALERT_COUNT
};

// Load Wi-Fi + topic from NVS and start the sender task. Call once, early in setup().
void alertsBegin();

// Queue an alert. Non-blocking, safe to call from the lock code.
void sendAlert(AlertEvent e);

// Set true while the solenoid is energised; the sender waits for false
// before each network step, so Wi-Fi bursts do not overlap the lock pulse.
void alertsLockBusy(bool busy);

// Call right before ESP.restart() in the wipe. Stores a flag in NVS; the
// "wipe done" alert is sent after the reboot, then the flag is cleared.
void alertsMarkWipeDone();

// ---------------- Wi-Fi setup (used by the Bluetooth code) ----------------
// Status text reported to the app (first line):
//   none | saved:<ssid> | scanning | scan:done (+ lines "rssi,locked,ssid")
//   testing | ok:<ssid> | fail:notfound | fail:password | fail:timeout | fail:busy
bool   alertsWifiScan();                                   // queue a scan
bool   alertsWifiSet(const char *ssid, const char *pass);  // queue a connection test
String alertsWifiStatus();                                 // current status text
const char *alertsTopic();                                 // this lock's alert topic
void   alertsOnStatus(void (*cb)(const char *status));     // called on every status change

// Print free heap / lowest-ever free heap with a label (memory measurements).
void logHeap(const char *where);
