// Push alert sender + Wi-Fi setup. See alerts.h for the overview.
#include "alerts.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_random.h>

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

// ---------------- Saved network settings (NVS "net") ----------------
static char netSsid[33]  = "";       // saved Wi-Fi name ("" = not set up)
static char netPass[65]  = "";       // saved Wi-Fi password (never logged)
static char netTopic[48] = "";       // this lock's alert topic
static char pendSsid[33] = "";       // Wi-Fi details waiting to be tested
static char pendPass[65] = "";
static SemaphoreHandle_t netMutex = nullptr;

static String status = "none";        // last status shown to the app
static void (*statusCb)(const char *) = nullptr;
static volatile bool netJobQueued = false;   // a scan or test is pending/running

enum JobType : uint8_t { JOB_ALERT, JOB_SCAN, JOB_TEST };
struct Job { uint8_t type; uint8_t event; };
static QueueHandle_t jobQueue = nullptr;

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

// status is written by the Wi-Fi task and read by the Bluetooth code: guard it.
static void setStatus(const String &s) {
  xSemaphoreTake(netMutex, portMAX_DELAY);
  status = s;
  xSemaphoreGive(netMutex);
  if (statusCb) statusCb(s.c_str());
}

String alertsWifiStatus() {
  xSemaphoreTake(netMutex, portMAX_DELAY);
  String s = status;
  xSemaphoreGive(netMutex);
  return s;
}
const char *alertsTopic()                           { return netTopic; }
void        alertsOnStatus(void (*cb)(const char *)) { statusCb = cb; }

static void waitLockFree() {
  while (lockBusy) vTaskDelay(pdMS_TO_TICKS(20));
}

// ---------------- Queue entry points (non-blocking) ----------------
void sendAlert(AlertEvent e) {
  if (!jobQueue || e >= ALERT_COUNT || !ALERTS_ENABLED) return;
  uint32_t now = millis();
  if (everSent[e] && now - lastSentMs[e] < ALERT_RATE_LIMIT_MS) {
    ALOG("[alert] %s rate-limited\n", EVENT_NAME[e]);
    return;
  }
  everSent[e]   = true;
  lastSentMs[e] = now;
  Job j = {JOB_ALERT, (uint8_t)e};
  if (xQueueSend(jobQueue, &j, 0) != pdTRUE) ALOG("[alert] queue full, %s dropped\n", EVENT_NAME[e]);
}

static bool queueNetJob(uint8_t type) {
  if (!jobQueue || netJobQueued) return false;   // one scan/test at a time
  Job j = {type, 0};
  if (xQueueSend(jobQueue, &j, 0) != pdTRUE) { setStatus("fail:busy"); return false; }
  netJobQueued = true;
  return true;
}

bool alertsWifiScan() { return queueNetJob(JOB_SCAN); }

bool alertsWifiSet(const char *ssid, const char *pass) {
  if (!ssid || !ssid[0] || strlen(ssid) > 32 || strlen(pass) > 64) return false;
  if (netJobQueued) return false;
  xSemaphoreTake(netMutex, portMAX_DELAY);
  strlcpy(pendSsid, ssid, sizeof(pendSsid));
  strlcpy(pendPass, pass, sizeof(pendPass));
  xSemaphoreGive(netMutex);
  return queueNetJob(JOB_TEST);
}

// ---------------- Wi-Fi helpers ----------------
static volatile uint8_t lastDiscReason = 0;

static void onWifiEvent(arduino_event_id_t, arduino_event_info_t info) {
  lastDiscReason = info.wifi_sta_disconnected.reason;
}

static void wifiDown() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  ALOG("[net] wifi off\n");
}

// Connects to the given network. Returns true when connected.
static bool wifiConnect(const char *ssid, const char *pass, uint32_t timeoutMs) {
  waitLockFree();
  logHeap("wifi start");
  lastDiscReason = 0;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    // Clear answers from the access point: stop early instead of waiting.
    uint8_t r = lastDiscReason;
    bool definite = (r == WIFI_REASON_NO_AP_FOUND || r == WIFI_REASON_AUTH_FAIL ||
                     r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || r == WIFI_REASON_HANDSHAKE_TIMEOUT);
    if (definite && millis() - t0 > 6000) break;
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  bool ok = WiFi.status() == WL_CONNECTED;
  ALOG("[net] wifi %s (%lu ms, reason %u)\n", ok ? "connected" : "FAILED",
       (unsigned long)(millis() - t0), (unsigned)lastDiscReason);
  logHeap("wifi up");
  return ok;
}

static const char *failReason() {
  switch (lastDiscReason) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
      return "fail:notfound";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "fail:password";
    default:
      return "fail:timeout";
  }
}

// ---------------- Scan / test jobs ----------------
static void runScan() {
  waitLockFree();
  setStatus("scanning");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  int n = WiFi.scanNetworks();
  ALOG("[net] scan found %d\n", n);

  // Unique names, strongest signal first, up to 12.
  const int MAXN = 12;
  String names[MAXN];
  int32_t rssi[MAXN];
  bool locked[MAXN];
  int count = 0;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length() || s.indexOf('\n') >= 0) continue;
    int k = 0;
    while (k < count && names[k] != s) k++;
    if (k < count) { if (WiFi.RSSI(i) > rssi[k]) rssi[k] = WiFi.RSSI(i); continue; }
    if (count < MAXN) {
      names[count] = s; rssi[count] = WiFi.RSSI(i);
      locked[count] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      count++;
    } else {                                   // full: replace the weakest if stronger
      int w = 0;
      for (int m = 1; m < MAXN; m++) if (rssi[m] < rssi[w]) w = m;
      if (WiFi.RSSI(i) > rssi[w]) {
        names[w] = s; rssi[w] = WiFi.RSSI(i); locked[w] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      }
    }
  }
  WiFi.scanDelete();
  for (int a = 0; a < count; a++)              // sort by signal, strongest first
    for (int b = a + 1; b < count; b++)
      if (rssi[b] > rssi[a]) {
        String t = names[a]; names[a] = names[b]; names[b] = t;
        int32_t r = rssi[a]; rssi[a] = rssi[b]; rssi[b] = r;
        bool l = locked[a]; locked[a] = locked[b]; locked[b] = l;
      }

  String out = "scan:done";
  for (int i = 0; i < count; i++)
    out += "\n" + String(rssi[i]) + "," + (locked[i] ? "1" : "0") + "," + names[i];
  setStatus(out);
}

static void runTest() {
  char ssid[33], pass[65];
  xSemaphoreTake(netMutex, portMAX_DELAY);
  strlcpy(ssid, pendSsid, sizeof(ssid));
  strlcpy(pass, pendPass, sizeof(pass));
  memset(pendPass, 0, sizeof(pendPass));
  xSemaphoreGive(netMutex);

  setStatus("testing");
  if (WiFi.status() == WL_CONNECTED) WiFi.disconnect(true);
  bool ok = wifiConnect(ssid, pass, ALERT_WIFI_TIMEOUT_MS);
  if (ok) {
    Preferences p;
    p.begin("net", false);
    p.putString("ssid", ssid);
    p.putString("pass", pass);
    p.end();
    xSemaphoreTake(netMutex, portMAX_DELAY);
    strlcpy(netSsid, ssid, sizeof(netSsid));
    strlcpy(netPass, pass, sizeof(netPass));
    xSemaphoreGive(netMutex);
    setStatus(String("ok:") + ssid);
  } else {
    WiFi.disconnect(true);
    setStatus(failReason());
  }
  memset(pass, 0, sizeof(pass));
}

// ---------------- Alerts (need secrets.h) ----------------
#if ALERTS_ENABLED
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"

static String  accessToken;          // cached OAuth token (never logged)
static time_t  tokenExpiry = 0;

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

static bool wifiUpSaved() {
  if (WiFi.status() == WL_CONNECTED) return true;
  char ssid[33], pass[65];
  xSemaphoreTake(netMutex, portMAX_DELAY);
  strlcpy(ssid, netSsid, sizeof(ssid));
  strlcpy(pass, netPass, sizeof(pass));
  xSemaphoreGive(netMutex);
  bool ok = ssid[0] && wifiConnect(ssid, pass, ALERT_WIFI_TIMEOUT_MS);
  memset(pass, 0, sizeof(pass));
  return ok;
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
  String json = String("{\"message\":{\"topic\":\"") + netTopic + "\"," +
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
  if (!netSsid[0]) {
    ALOG("[alert] no Wi-Fi set up: %s not sent\n", EVENT_NAME[e]);
    return false;
  }
  for (int attempt = 1; attempt <= 2; attempt++) {
    if (wifiUpSaved() && haveTime() && getToken() && postFcm(e)) return true;
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
  return false;
}
#else
static bool deliver(AlertEvent) { return false; }
#endif

// ---------------- The task ----------------
static void netTask(void *) {
  Job j;
  for (;;) {
    if (xQueueReceive(jobQueue, &j, portMAX_DELAY) != pdTRUE) continue;
    if (j.type == JOB_SCAN) {
      runScan();
      netJobQueued = false;
    } else if (j.type == JOB_TEST) {
      runTest();
      netJobQueued = false;
    } else {
      AlertEvent e = (AlertEvent)j.event;
      ALOG("[alert] sending %s\n", EVENT_NAME[e]);
      bool ok = deliver(e);
      if (ok && e == ALERT_WIPE_DONE) {
        Preferences p;
        p.begin("alerts", false);
        p.remove("wipeDone");
        p.end();
      }
    }
    // Keep Wi-Fi up only while more jobs are waiting.
    if (uxQueueMessagesWaiting(jobQueue) == 0) wifiDown();
    ALOG("[net] task stack high-water mark: %u bytes free\n",
         (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    logHeap("job done");
  }
}

// ---------------- Start-up ----------------
static void loadNet() {
  Preferences p;
  p.begin("net", false);
  String topic = p.getString("topic", "");
  if (!topic.length()) {
#ifdef FCM_TOPIC
    topic = FCM_TOPIC;                        // keeps an existing lock + app pairing working
#else
    char t[40] = "rk-";
    for (int i = 0; i < 4; i++) {
      char h[9];
      snprintf(h, sizeof(h), "%08lx", (unsigned long)esp_random());
      strlcat(t, h, sizeof(t));
    }
    topic = t;
#endif
    p.putString("topic", topic);
  }
  strlcpy(netTopic, topic.c_str(), sizeof(netTopic));

  String ssid = p.getString("ssid", "");
  String pass = p.getString("pass", "");
#if PROVISION_FROM_SECRETS && defined(WIFI_SSID) && defined(WIFI_PASS)
  if (!ssid.length()) {
    ssid = WIFI_SSID;
    pass = WIFI_PASS;
    p.putString("ssid", ssid);
    p.putString("pass", pass);
  }
#endif
  p.end();
  strlcpy(netSsid, ssid.c_str(), sizeof(netSsid));
  strlcpy(netPass, pass.c_str(), sizeof(netPass));
  status = netSsid[0] ? String("saved:") + netSsid : String("none");
}

void alertsBegin() {
  netMutex = xSemaphoreCreateMutex();
  loadNet();
  WiFi.onEvent(onWifiEvent, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  jobQueue = xQueueCreate(8, sizeof(Job));
  xTaskCreatePinnedToCore(netTask, "net", ALERT_TASK_STACK, nullptr, 1, nullptr, 0);

#if ALERTS_ENABLED
  Preferences p;
  p.begin("alerts", true);
  bool wipeDone = p.getBool("wipeDone", false);
  p.end();
  if (wipeDone) sendAlert(ALERT_WIPE_DONE);
#if ALERT_TEST_ON_BOOT
  sendAlert(ALERT_TEST);
#endif
  ALOG("[alert] sender ready, wifi %s\n", netSsid[0] ? "set" : "NOT set");
#else
  ALOG("[alert] secrets.h missing: alerts disabled, Wi-Fi setup still works\n");
#endif
}
