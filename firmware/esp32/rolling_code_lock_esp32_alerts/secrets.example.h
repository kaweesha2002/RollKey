// TEMPLATE. Copy this file to secrets.h (same folder) and fill it in,
// or generate secrets.h with: python firmware/esp32/tools/make_secrets.py
// secrets.h is git-ignored. Never commit it, print it or paste it anywhere.
#pragma once

#define WIFI_SSID        "your-wifi-name"
#define WIFI_PASS        "your-wifi-password"

// From the service account JSON key (role: Firebase Cloud Messaging API Admin)
#define FCM_PROJECT_ID   "your-firebase-project-id"
#define FCM_CLIENT_EMAIL "fcm-sender@your-firebase-project-id.iam.gserviceaccount.com"

// Random topic name. The Android app must subscribe to the SAME name.
#define FCM_TOPIC        "lock-alerts-CHANGE-ME-random"

// The "private_key" value from the JSON, with real line breaks.
static const char FCM_PRIVATE_KEY[] = R"KEY(-----BEGIN PRIVATE KEY-----
PASTE THE KEY LINES HERE
-----END PRIVATE KEY-----
)KEY";
