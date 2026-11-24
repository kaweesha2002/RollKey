// Push alert sender. See alerts.h for the overview.
#include "alerts.h"
#include <Preferences.h>

#if __has_include("secrets.h")
  #include "secrets.h"
  #define ALERTS_ENABLED 1
#else
  #define ALERTS_ENABLED 0
#endif

#if ALERTS_LOG
  #define ALOG(...)   Serial.printf(__VA_ARGS__)
#else
  #define ALOG(...)
#endif

static volatile bool lockBusy = false;
static uint32_t lastSentMs[ALERT_COUNT];
static bool     everSent[ALERT_COUNT];

static const char *const EVENT_NAME[ALERT_COUNT] = {
  "test", "wrong_code", "lockout", "hard_lockout", "unlocked", "wipe_attempt", "wipe_done"
};
static const char *const EVENT_TEXT[ALERT_COUNT] = {
  "Test alert from the lock",
  "Wrong code entered",
  "Lock blocked after wrong codes",
  "Lock fully locked out",
  "Safe unlocked",
  "Wipe attempted",
  "Lock was wiped"
};

void logHeap(const char *where) {
  ALOG("[heap] %-16s free=%6u  min=%6u  largest=%6u\n", where,
       (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
       (unsigned)ESP.getMaxAllocHeap());
}

void alertsLockBusy(bool busy) { lockBusy = busy; }

void alertsMarkWipeDone() {
  Preferences p;
  p.begin("alerts", false);
  p.putBool("wipeDone", true);
  p.end();
}

#if !ALERTS_ENABLED
// ------------- No secrets.h: alerts compiled out -------------
void alertsBegin() { ALOG("[alert] secrets.h missing: alerts disabled\n"); }
void sendAlert(AlertEvent) {}

#else
// ------------- Full sender -------------
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <esp_random.h>
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"

static QueueHandle_t alertQueue = nullptr;
static String  accessToken;          // cached OAuth token (never logged)
static time_t  tokenExpiry = 0;

void sendAlert(AlertEvent e) {
  if (!alertQueue || e >= ALERT_COUNT) return;
  uint32_t now = millis();
  if (everSent[e] && now - lastSentMs[e] < ALERT_RATE_LIMIT_MS) {
    ALOG("[alert] %s rate-limited\n", EVENT_NAME[e]);
    return;
  }
  everSent[e]   = true;
  lastSentMs[e] = now;
  uint8_t v = e;
  if (xQueueSend(alertQueue, &v, 0) != pdTRUE) ALOG("[alert] queue full, %s dropped\n", EVENT_NAME[e]);
}

static void waitLockFree() {
  while (lockBusy) vTaskDelay(pdMS_TO_TICKS(20));
}

// ---------- helpers ----------
static int espRng(void *, unsigned char *buf, size_t len) {
  esp_fill_random(buf, len);
  return 0;
}

static String b64url(const uint8_t *data, size_t len) {
  size_t olen = 0;
  size_t cap = 4 * ((len + 2) / 3) + 1;
  unsigned char *out = (unsigned char *)malloc(cap);
  if (!out) return String();
  mbedtls_base64_encode(out, cap, &olen, data, len);
  String s;
  s.reserve(olen);
  for (size_t i = 0; i < olen; i++) {
    char c = out[i];
    if (c == '+') c = '-';
    else if (c == '/') c = '_';
    else if (c == '=') break;
    s += c;
  }
  free(out);
  return s;
}

static bool wifiUp() {
  if (WiFi.status() == WL_CONNECTED) return true;
  waitLockFree();
  logHeap("wifi start");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < ALERT_WIFI_TIMEOUT_MS) vTaskDelay(pdMS_TO_TICKS(200));
  bool ok = WiFi.status() == WL_CONNECTED;
  ALOG("[alert] wifi %s (%lu ms)\n", ok ? "connected" : "FAILED", (unsigned long)(millis() - t0));
  logHeap("wifi up");
  return ok;
}

static void wifiDown() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  ALOG("[alert] wifi off\n");
}

static bool haveTime() {
  if (time(nullptr) > 1700000000) return true;
  configTime(0, 0, "time.google.com", "pool.ntp.org");
  for (int i = 0; i < 50; i++) {
    if (time(nullptr) > 1700000000) return true;
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  ALOG("[alert] NTP time FAILED\n");
  return false;
}

// Signs the service-account JWT (RS256). Returns "" on failure.
static String makeJwt(time_t now) {
  String header = b64url((const uint8_t *)"{\"alg\":\"RS256\",\"typ\":\"JWT\"}", 27);
  String claims = String("{\"iss\":\"") + FCM_CLIENT_EMAIL +
                  "\",\"scope\":\"https://www.googleapis.com/auth/firebase.messaging\"" +
                  ",\"aud\":\"https://oauth2.googleapis.com/token\"" +
                  ",\"iat\":" + String((uint32_t)now) +
                  ",\"exp\":" + String((uint32_t)(now + 3600)) + "}";
  String signingInput = header + "." + b64url((const uint8_t *)claims.c_str(), claims.length());

  uint8_t hash[32];
  mbedtls_sha256((const unsigned char *)signingInput.c_str(), signingInput.length(), hash, 0);

  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  String jwt;
  uint8_t *sig = (uint8_t *)malloc(MBEDTLS_PK_SIGNATURE_MAX_SIZE);
  size_t sigLen = 0;
  int rc = sig ? mbedtls_pk_parse_key(&pk, (const unsigned char *)FCM_PRIVATE_KEY, sizeof(FCM_PRIVATE_KEY),
                                      nullptr, 0, espRng, nullptr) : -1;
  if (rc == 0) rc = mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash),
                                    sig, MBEDTLS_PK_SIGNATURE_MAX_SIZE, &sigLen, espRng, nullptr);
  if (rc == 0) jwt = signingInput + "." + b64url(sig, sigLen);
  else         ALOG("[alert] JWT signing FAILED (mbedtls %d)\n", rc);
  mbedtls_pk_free(&pk);
  free(sig);
  return jwt;
}

static String jsonField(const String &body, const char *key) {
  String k = String("\"") + key + "\"";
  int i = body.indexOf(k);
  if (i < 0) return String();
  i = body.indexOf(':', i + k.length());
  if (i < 0) return String();
  i++;
  while (i < (int)body.length() && (body[i] == ' ' || body[i] == '"')) i++;
  int j = i;
  while (j < (int)body.length() && body[j] != '"' && body[j] != ',' && body[j] != '}') j++;
  return body.substring(i, j);
}

static bool getToken() {
  time_t now = time(nullptr);
  if (accessToken.length() && now < tokenExpiry - 120) return true;   // reuse cached token

  String jwt = makeJwt(now);
  if (!jwt.length()) return false;

  waitLockFree();
  NetworkClientSecure client;
  client.useBuiltinCACertBundle();
  HTTPClient http;
  http.begin(client, "https://oauth2.googleapis.com/token");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  logHeap("tls token");
  int code = http.POST("grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer&assertion=" + jwt);
  String body = http.getString();
  http.end();
  logHeap("tls token done");

  if (code != 200) {
    char err[100] = "";
    client.lastError(err, sizeof(err));
    ALOG("[alert] token request FAILED: HTTP %d  tls: %s\n", code, err);
    return false;
  }
  accessToken = jsonField(body, "access_token");
  long ttl = jsonField(body, "expires_in").toInt();
  tokenExpiry = now + (ttl > 0 ? ttl : 3000);
  ALOG("[alert] token OK, valid %ld s\n", ttl);
  return accessToken.length() > 0;
}

static bool postFcm(AlertEvent e) {
  String url = String("https://fcm.googleapis.com/v1/projects/") + FCM_PROJECT_ID + "/messages:send";
  String json = String("{\"message\":{\"topic\":\"") + FCM_TOPIC + "\"," +
                "\"notification\":{\"title\":\"RollKey\",\"body\":\"" + EVENT_TEXT[e] + "\"}," +
                "\"data\":{\"event\":\"" + EVENT_NAME[e] + "\"}," +
                "\"android\":{\"priority\":\"HIGH\",\"ttl\":\"3600s\"," +
                "\"notification\":{\"channel_id\":\"lock_alerts\"}}}}";

  waitLockFree();
  NetworkClientSecure client;
  client.useBuiltinCACertBundle();
  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + accessToken);
  logHeap("tls fcm");
  int code = http.POST(json);
  String body = code > 0 && code != 200 ? http.getString() : String();
  http.end();
  logHeap("tls fcm done");

  if (code == 401) tokenExpiry = 0;                 // token rejected: re-sign next time
  if (code != 200) {
    char err[100] = "";
    client.lastError(err, sizeof(err));
    ALOG("[alert] FCM send FAILED: HTTP %d  tls: %s  %.160s\n", code, err, body.c_str());
    return false;
  }
  ALOG("[alert] sent: %s\n", EVENT_NAME[e]);
  return true;
}

static bool deliver(AlertEvent e) {
  for (int attempt = 1; attempt <= 2; attempt++) {
    if (wifiUp() && haveTime() && getToken() && postFcm(e)) return true;
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
  return false;
}

static void alertTask(void *) {
  uint8_t v;
  for (;;) {
    if (xQueueReceive(alertQueue, &v, portMAX_DELAY) != pdTRUE) continue;
    AlertEvent e = (AlertEvent)v;
    ALOG("[alert] sending %s\n", EVENT_NAME[e]);
    bool ok = deliver(e);
    if (ok && e == ALERT_WIPE_DONE) {
      Preferences p;
      p.begin("alerts", false);
      p.remove("wipeDone");
      p.end();
    }
    // Keep Wi-Fi up only while more alerts are waiting.
    if (uxQueueMessagesWaiting(alertQueue) == 0) wifiDown();
    ALOG("[alert] task stack high-water mark: %u bytes free\n",
         (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    logHeap("alert done");
  }
}

void alertsBegin() {
  alertQueue = xQueueCreate(8, sizeof(uint8_t));
  xTaskCreatePinnedToCore(alertTask, "alerts", ALERT_TASK_STACK, nullptr, 1, nullptr, 0);

  Preferences p;
  p.begin("alerts", true);
  bool wipeDone = p.getBool("wipeDone", false);
  p.end();
  if (wipeDone) sendAlert(ALERT_WIPE_DONE);
#if ALERT_TEST_ON_BOOT
  sendAlert(ALERT_TEST);
#endif
  ALOG("[alert] sender ready\n");
}
#endif
