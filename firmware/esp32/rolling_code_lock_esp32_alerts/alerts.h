// Push alerts for the rolling code lock.
//
// The lock calls sendAlert(event) with one line at each place an event
// happens. sendAlert() never blocks: it only rate-limits and drops the event
// into a queue. A separate FreeRTOS task (on core 0) does the slow work:
//   Wi-Fi on -> NTP time -> OAuth token (signed JWT, cached ~1 h)
//   -> HTTPS POST to FCM HTTP v1 (topic message) -> Wi-Fi off
//
// Only the event type is ever sent or logged: never a code, PIN, pair key,
// token or any other secret.
//
// Secrets (Wi-Fi, service account, topic) live in secrets.h, which is
// git-ignored. Copy secrets.example.h to secrets.h and fill it in, or run
// firmware/esp32/tools/make_secrets.py. Without secrets.h the sketch still
// builds, with alerts switched off.
#pragma once
#include <Arduino.h>

// ---------------- Settings ----------------
#define ALERTS_LOG          0        // 1 = print alert progress + heap numbers to Serial
#define ALERT_TEST_ON_BOOT  0        // 1 = send one "test" alert after every boot (bench test)
#define ALERT_RATE_LIMIT_MS 30000UL  // at most one alert per event type every 30 s
#define ALERT_WIFI_TIMEOUT_MS 15000UL
#define ALERT_TASK_STACK    8192     // bytes; measured peak ~4.5 KB during a full HTTPS send

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

// Start the sender task. Call once, early in setup().
void alertsBegin();

// Queue an alert. Non-blocking, safe to call from the lock code.
void sendAlert(AlertEvent e);

// Set true while the solenoid is energised; the sender waits for false
// before each network step, so Wi-Fi bursts do not overlap the lock pulse.
void alertsLockBusy(bool busy);

// Call right before ESP.restart() in the wipe. Stores a flag in NVS; the
// "wipe done" alert is sent after the reboot, then the flag is cleared.
void alertsMarkWipeDone();

// Print free heap / lowest-ever free heap with a label (memory measurements).
void logHeap(const char *where);
