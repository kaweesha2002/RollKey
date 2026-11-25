// TEMPLATE. Copy this file to secrets.h (same folder) and fill it in,
// or generate secrets.h with: python firmware/esp32/tools/make_secrets.py
// secrets.h is git-ignored. Never commit it, print it or paste it anywhere.
#pragma once

// ---- Required for push alerts: from the service account JSON key ----
// (role: Firebase Cloud Messaging API Admin)
#define FCM_PROJECT_ID   "your-firebase-project-id"
#define FCM_CLIENT_EMAIL "fcm-sender@your-firebase-project-id.iam.gserviceaccount.com"

// The "private_key" value from the JSON, with real line breaks.
static const char FCM_PRIVATE_KEY[] = R"KEY(-----BEGIN PRIVATE KEY-----
PASTE THE KEY LINES HERE
-----END PRIVATE KEY-----
)KEY";

// ---- Optional, development only ----
// Normally the owner sets Wi-Fi from the RollKey app, and each lock makes its
// own random alert topic. Define these only to pre-load a bench lock
// (used when PROVISION_FROM_SECRETS is 1 in alerts.h and nothing is saved yet).
// #define WIFI_SSID     "your-2.4GHz-wifi"
// #define WIFI_PASS     "your-wifi-password"
// #define FCM_TOPIC     "lock-alerts-..."   // keep an older lock's topic
