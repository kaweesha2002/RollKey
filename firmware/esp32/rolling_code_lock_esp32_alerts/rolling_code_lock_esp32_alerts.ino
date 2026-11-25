/*
 * ESP32 Rolling Code Lock — final + push alerts (OLED, pull-in/hold solenoid, BLE, FCM)
 *
 * PUSH ALERTS (alerts.h / alerts.cpp): wrong code, lockout, hard lockout,
 * unlocked, wipe attempted, wipe done -> FCM topic message, sent from a
 * background task over Wi-Fi. Needs secrets.h (see secrets.example.h);
 * without it the sketch builds with alerts off. Partition: Huge APP.
 * BLE uses the NimBLE-Arduino library (Bluedroid left too little RAM for HTTPS).
 * Enrollment / dark standby / code entry / unlock / deadlock / gated wipe
 *
 * Board: ESP32 Dev Module (30-pin DevKit, WROOM)
 * Library needed: U8g2 (install from the Library Manager)
 *
 * Hardware (see the schematic):
 *   OLED (I2C)      SDA GPIO21, SCL GPIO22, powered from the ESP32 3V3 pin
 *   Keypad 4x4      rows GPIO13, 14, 27, 26; columns GPIO25, 33, 32, 16
 *   Red / green LED GPIO19 / GPIO23, each through 220 ohm
 *   Buzzer          GPIO18
 *   Solenoid gate   GPIO4 -> 220 ohm -> IRLZ44N (PWM pull-in, then hold)
 *   Power           12 V bus from the UPS; 5 V from the buck converter to VIN
 *                   (the UPS is hardware only: no code is involved)
 *
 * Owner enrollment (first boot only, permanent):
 *   - WIPE PIN      : 4 characters, each 0-9 or A-D, entered after '*'
 *                     (14^4 = 38416 combinations, e.g. A12B)
 *   - RESTORE NUMBER: 4 digits, 0-9 only, the code the lock returns to
 *                     after a wipe
 *   Both live in the "owner" NVS namespace, which the wipe never touches.
 *   There is no change path. A forgotten wipe PIN cannot be recovered.
 *   To start over: Tools -> Erase All Flash Before Sketch Upload (one upload).
 *
 * Letters are accepted in the wipe PIN only. The rolling code stays numeric:
 * code digits come from makeDecimal(), which folds nibbles A-F down to 0-5,
 * so an 'A' could never be distinguished from a '0' on entry. A PIN holding
 * at least one letter therefore can never equal any rolling code.
 *
 * Design notes:
 *   - seed == 0 is clamped to the restore number (an LFSR would stay at 0)
 *   - explicit "init" flag in NVS replaces the 0xFFFF blank sentinel
 *   - factory wipe fires LED + buzzer only; the solenoid stays off
 *   - deadlock challenge digits come from the hardware RNG, not seed_L
 *   - 10s inter-key timeout; never counts as a failed attempt
 *   - wipe is gated by the owner PIN; wrong PIN counts toward the deadlock
 *   - deadlock recovery is capped at 2 attempts, then hard lockout
 *   - wipe resets the code to the restore number, forgets all paired phones,
 *     erases the Bluetooth pair key and restarts; the lock then asks for a
 *     new pair key on the keypad
 *   - the OLED sleeps (display off) in standby and wakes on '#' or '*'
 *   - solenoid: full power for SOLENOID_PULL_MS, then PWM hold at
 *     SOLENOID_HOLD_DUTY until SOLENOID_OPEN_MS has passed, then released;
 *     the screen shows OPEN for that time and then goes dark
 *
 * Post-deadlock flow:
 *   - Successful deadlock recovery does NOT open the lock. It shows
 *     "PRESS # + CODE" for 2.5 s, then goes dark. Only a correct rolling
 *     code entered through codeEntry() fires the solenoid.
 *   - postDeadlockPending gives the first code attempt after recovery a
 *     grace pass: a wrong code does not increment failCount, so the user
 *     is not double-penalised. The flag clears after that one attempt.
 *
 * Bluetooth (BLE):
 *   - The current code is published on a BLE characteristic (READ + NOTIFY)
 *     at boot and after every successful unlock; the lock advertises as
 *     "RollingLock". Read it with the companion app. With BLE_ALWAYS_ON = 1
 *     (default) it advertises all the time; with 0 only for BLE_WINDOW_MS
 *     (60 s) after an unlock. Only phones paired with the lock's 6-digit
 *     PAIR KEY can read it.
 *   - The code is never shown on the LCD, only in the app.
 *   - PAIRING REQUIRED: the characteristic can only be read by a phone that
 *     has paired using the 6-digit PAIR KEY. The key is typed on the lock's
 *     keypad (first setup, and again after every system wipe) and stored in
 *     NVS - it is not in the source code. The phone asks for it once; after
 *     that it stays bonded.
 *   - SYSTEM WIPE also forgets every paired phone and erases the pair key;
 *     the lock restarts and asks for a new key on the keypad.
 *   - Upload tip: if the sketch is too big, choose
 *     Tools -> Partition Scheme -> Huge APP.
 *
 * DEBUG: set to 1 while testing to see seed/code/challenge answers on the
 * Serial Monitor. Set to 0 for the final build so nothing leaks over USB.
 */

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Keypad.h>
#include <Preferences.h>
#include <esp_system.h>
#include <string.h>
#include <NimBLEDevice.h>         // NimBLE stack: ~100 KB less RAM than Bluedroid (alerts need it)
#include "alerts.h"            // push alerts (separate task)

// ---------------- Debug switch ----------------
#define DEBUG 0          // 1 = testing, 0 = final build

#if DEBUG
  #define LOG(...)   Serial.print(__VA_ARGS__)
  #define LOGLN(...) Serial.println(__VA_ARGS__)
#else
  #define LOG(...)
  #define LOGLN(...)
#endif

// ---------------- Pin map ----------------
#define LED_RED   19      // error indicator
#define LED_GREEN 23      // unlock / wipe indicator
#define BUZZER    18      // passive piezo
#define SOLENOID_PIN 4    // GPIO4 -> 220 ohm -> IRLZ44N gate (10k gate -> GND)
// The OLED uses the I2C pins: SDA = GPIO21, SCL = GPIO22.

// ---------------- Solenoid: pull in, then hold ----------------
// The lock needs its full current (about 0.4 A) only for the first moment.
// After that the pin switches to PWM, so the coil sees a lower average
// current and stays cool. Tune SOLENOID_HOLD_DUTY on the real lock: lower it
// until the tongue drops out, then use about 1.5 times that value.
// Fit the flyback diode right at the lock (a Schottky such as the 1N5822).
#define SOLENOID_PULL_MS    300     // full power while the tongue pulls in
#define SOLENOID_HOLD_DUTY  100     // 0-255 (100 is about 40 %)
#define SOLENOID_OPEN_MS    3000    // total unlocked time
#define SOLENOID_PWM_HZ     20000   // above hearing, so no whine
static_assert(SOLENOID_OPEN_MS > SOLENOID_PULL_MS, "open time must exceed pull-in time");

void solenoidInit() { ledcAttach(SOLENOID_PIN, SOLENOID_PWM_HZ, 8); ledcWrite(SOLENOID_PIN, 0); }
void solenoidFull() { ledcWrite(SOLENOID_PIN, 255); }
void solenoidHold() { ledcWrite(SOLENOID_PIN, SOLENOID_HOLD_DUTY); }
void solenoidOff()  { ledcWrite(SOLENOID_PIN, 0); }

// ---------------- OLED display ----------------
#define OLED_SH1106   1       // 1 = SH1106 (many 1.3 inch modules), 0 = SSD1306
#define OLED_FLIP     1       // 1 = screen mounted upside down (rotate 180), 0 = normal
#define OLED_ROTATION (OLED_FLIP ? U8G2_R2 : U8G2_R0)
#define OLED_I2C_ADDR 0x3C    // 0x3D on a few boards

#if OLED_SH1106
  U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(OLED_ROTATION, U8X8_PIN_NONE);
#else
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(OLED_ROTATION, U8X8_PIN_NONE);
#endif

// Presents the OLED like the 16x2 text display the sketch was written for,
// so every lcd.xxx() call in the sketch keeps working unchanged.
class TextOled {
 public:
  void init() {
    u8g2.setI2CAddress(OLED_I2C_ADDR << 1);
    u8g2.begin();
    u8g2.setBusClock(400000);
    u8g2.setFont(u8g2_font_8x13B_tf);   // 8 pixels wide: 16 columns fit
    clear();
  }
  void backlight()   { u8g2.setPowerSave(0); }
  void noBacklight() {}                 // an OLED has no backlight
  void display()     { on = true;  u8g2.setPowerSave(0); draw(); }
  void noDisplay()   { on = false; u8g2.setPowerSave(1); }
  void clear()       { memset(buf, ' ', sizeof(buf)); col = 0; row = 0; draw(); }
  void setCursor(uint8_t c, uint8_t r) { col = c; row = r; }
  void print(char c)        { put(c); draw(); }
  void print(const char *s) { while (*s) put(*s++); draw(); }
  void print(int v)         { char t[12]; snprintf(t, sizeof(t), "%d", v); print(t); }

 private:
  char    buf[2][16];
  uint8_t col = 0, row = 0;
  bool    on  = true;

  void put(char c) { if (row < 2 && col < 16) buf[row][col++] = c; }
  void draw() {
    if (!on) return;
    char line[17];
    u8g2.clearBuffer();
    for (uint8_t r = 0; r < 2; r++) {
      memcpy(line, buf[r], 16);
      line[16] = 0;
      u8g2.drawStr(0, 26 + r * 28, line);
    }
    u8g2.sendBuffer();
  }
};

TextOled lcd;
Preferences prefs;

// ---------------- Keypad (4x4) ----------------
const byte ROWS = 4;
const byte COLS = 4;
byte rowPins[ROWS] = {13, 14, 27, 26};
byte colPins[COLS] = {25, 33, 32, 16};

char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ---------------- Constants ----------------
#define LFSR_TAP          0xB4
#define CODE_LEN          4
#define PIN_LEN           4          // 4 chars, 0-9 and A-D, e.g. A12B
#define FAIL_LIMIT        2
#define DEADLOCK_LIMIT    2
#define INPUT_TIMEOUT_MS  10000UL
#define NO_TIMEOUT        0UL
#define INPUT_TIMED_OUT   (-1)
#define FALLBACK_SEED     0x1357     // guards a corrupt restore value only

#define MAX_FIELD_LEN     4          // longest input field
#define PAIR_LEN          6          // Bluetooth pair key: 6 digits

// ---------------- State ----------------
uint16_t seed        = 0;
uint16_t wipePin     = 0;            // 4 values packed as nibbles
uint16_t restoreSeed = 0;            // nibble-packed, e.g. 7391 -> 0x7391
bool     ownerSet    = false;
uint32_t pairKey     = 0;            // 6-digit Bluetooth pair key (100000-999999)
bool     pairKeySet  = false;
bool     clearBondsOnStart = false;  // set when a NEW pair key was just typed

uint8_t  entropy          = 0;       // mirrors r19
uint8_t  failCount        = 0;       // mirrors r23 — not persisted
uint8_t  deadlockAttempts = 0;
bool     inDeadlock       = false;
bool     hardLockout      = false;   // recovery exhausted

// Set true after a successful deadlock escape. The first code-entry
// attempt afterwards (pass or fail) is protected from adding to failCount.
bool     postDeadlockPending = false;

uint8_t  challengeA = 0;             // random, independent of seed
uint8_t  challengeB = 0;
bool     challengeReady = false;

// ---------------- Key decoding ----------------
// '0'-'9' -> 0..9, 'A'-'D' -> 10..13, anything else -> -1
int8_t keyToValue(char k) {
  if (k >= '0' && k <= '9') return (int8_t)(k - '0');
  if (k >= 'A' && k <= 'D') return (int8_t)(10 + (k - 'A'));
  return -1;
}

// ---------------- Seed maths ----------------
uint16_t safeRestore() {
  return (restoreSeed != 0) ? restoreSeed : (uint16_t)FALLBACK_SEED;
}

void guardSeed() {
  if (seed == 0) {
    seed = safeRestore();
    LOGLN("[guard] seed hit zero, reset to restore number");
  }
}

void lfsrUpdate() {
  uint8_t carry = (seed >> 15) & 1;
  seed <<= 1;
  if (carry) seed ^= LFSR_TAP;
}

uint8_t makeDecimal(uint8_t nib) {
  return (nib < 10) ? nib : (uint8_t)(nib - 10);
}

void currentCodeDigits(uint8_t out[CODE_LEN]) {
  out[0] = makeDecimal((seed >> 12) & 0x0F);
  out[1] = makeDecimal((seed >>  8) & 0x0F);
  out[2] = makeDecimal((seed >>  4) & 0x0F);
  out[3] = makeDecimal( seed        & 0x0F);
}

uint16_t digitsToSeed(const uint8_t *d) {
  return ((uint16_t)d[0] << 12) | ((uint16_t)d[1] << 8) |
         ((uint16_t)d[2] <<  4) |  (uint16_t)d[3];
}

uint16_t valuesToPin(const uint8_t *v) {
  return ((uint16_t)v[0] << 12) | ((uint16_t)v[1] << 8) |
         ((uint16_t)v[2] <<  4) |  (uint16_t)v[3];
}

void injectEntropy() {
  entropy ^= (uint8_t)(micros() & 0xFF);
}

// ---------------- Deadlock challenge ----------------
void newChallenge() {
  uint32_t r = esp_random() ^ ((uint32_t)entropy << 8) ^ micros();
  challengeA = (uint8_t)( r       % 10);
  challengeB = (uint8_t)((r / 10) % 10);
  challengeReady = true;
}

// ---------------- Storage ----------------
void saveOwner() {
  prefs.begin("owner", false);
  prefs.putUShort("pin", wipePin);
  prefs.putUShort("restore", restoreSeed);
  prefs.putBool("set", true);
  prefs.end();
}

void loadOwner() {
  prefs.begin("owner", true);
  ownerSet    = prefs.getBool("set", false);
  wipePin     = prefs.getUShort("pin", 0);
  restoreSeed = prefs.getUShort("restore", 0);
  prefs.end();
}

void savePairKey() {
  prefs.begin("ble", false);
  prefs.putUInt("key", pairKey);
  prefs.end();
}

void loadPairKey() {
  prefs.begin("ble", true);
  pairKey = prefs.getUInt("key", 0);
  prefs.end();
  pairKeySet = (pairKey >= 100000UL && pairKey <= 999999UL);
}

void clearPairKey() {
  prefs.begin("ble", false);
  prefs.clear();
  prefs.end();
  pairKey    = 0;
  pairKeySet = false;
}

void saveSeed() {
  prefs.begin("lock", false);
  prefs.putUShort("seed", seed);
  prefs.putBool("init", true);
  prefs.end();
}

void loadSeed() {
  prefs.begin("lock", true);
  bool initialised = prefs.getBool("init", false);
  uint16_t stored  = prefs.getUShort("seed", 0);
  prefs.end();

  if (!initialised) {
    seed = safeRestore();
    saveSeed();
    LOGLN("[nvs] lock blank, seeded from restore number");
  } else {
    seed = stored;                   // 0xFFFF is legal here
  }
  guardSeed();
}

// Clears the rolling seed only. The "owner" namespace is untouched.
void resetLockToRestore() {
  prefs.begin("lock", false);
  prefs.clear();
  prefs.end();

  seed = safeRestore();
  saveSeed();
}

// ---------------- LCD power ----------------
void lcdSleep() {
  lcd.clear();
  lcd.noDisplay();
  lcd.noBacklight();
}

void lcdWake() {
  lcd.display();
  lcd.backlight();
  lcd.clear();
}

// ---------------- Feedback ----------------
// Tones sit near the piezo's resonance (~2700 Hz, loudest in testing).
// Pitches keep their old order: reject < error < timeout < wipe < key click.
void beepOnce() {
  tone(BUZZER, 2000);
  delay(500);
  noTone(BUZZER);
}

void beepShort() {
  tone(BUZZER, 3200);
  delay(40);
  noTone(BUZZER);
}

// Sounded when a key is not valid in the current field
void beepReject() {
  tone(BUZZER, 1800);
  delay(90);
  noTone(BUZZER);
}

void timeoutTone() {
  tone(BUZZER, 2700); delay(80);  noTone(BUZZER);
  delay(60);
  tone(BUZZER, 2200); delay(120); noTone(BUZZER);
}

void wipeTone() {
  for (uint8_t i = 0; i < 3; i++) {
    tone(BUZZER, 2700); delay(150); noTone(BUZZER);
    delay(100);
  }
}

void showSeedDigits() {
  uint8_t d[CODE_LEN];
  currentCodeDigits(d);
  for (uint8_t i = 0; i < CODE_LEN; i++) lcd.print((char)('0' + d[i]));
}

void logCode(const char *tag) {
#if DEBUG
  uint8_t d[CODE_LEN];
  currentCodeDigits(d);
  LOG(tag);
  LOG(" seed=0x");
  LOG(seed, HEX);
  LOG("  code=");
  for (uint8_t i = 0; i < CODE_LEN; i++) LOG(d[i]);
  LOGLN();
#else
  (void)tag;
#endif
}

void flashMessage(const char *l0, const char *l1, uint16_t ms) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(l0);
  if (l1) { lcd.setCursor(0, 1); lcd.print(l1); }
  delay(ms);
}

// Standby is dark: display and backlight both off.
void standby() {
  lcdSleep();
}

// ---------------- Input ----------------
// Up to maxLen keys, '#' terminates early. Returns count, or
// INPUT_TIMED_OUT if timeoutMs elapses between presses (0 = wait forever).
// allowLetters admits A-D as values 10-13; otherwise they are rejected.
int8_t getInputString(uint8_t *buf, uint8_t maxLen,
                      uint8_t inRow, uint8_t inCol,
                      uint8_t cdRow, uint8_t cdCol,
                      uint32_t timeoutMs, bool mask, bool allowLetters) {
  uint8_t  n     = 0;
  uint32_t last  = millis();
  int16_t  shown = -1;

  while (n < maxLen) {
    if (timeoutMs > 0) {
      uint32_t elapsed = millis() - last;
      if (elapsed >= timeoutMs) return INPUT_TIMED_OUT;

      int16_t remain = (int16_t)(((timeoutMs - elapsed) + 999UL) / 1000UL);
      if (remain != shown) {
        shown = remain;
        lcd.setCursor(cdCol, cdRow); lcd.print("  ");
        lcd.setCursor(cdCol, cdRow); lcd.print((int)remain);
      }
    }

    char k = keypad.getKey();
    if (!k) { delay(5); continue; }

    last  = millis();                // restart the window on every press
    shown = -1;
    injectEntropy();

    if (k == '#') { beepShort(); break; }

    int8_t v = keyToValue(k);
    bool   isLetter = (k >= 'A' && k <= 'D');

    // '*' and, where disallowed, letters are not values
    if (v < 0 || (isLetter && !allowLetters)) {
      beepReject();
      delay(120);
      continue;
    }

    beepShort();
    buf[n++] = (uint8_t)v;
    lcd.setCursor(inCol + n - 1, inRow);
    lcd.print(mask ? '*' : k);
    delay(150);
  }
  return (int8_t)n;
}

// Blocks until exactly len values are entered. Enrollment only.
void readExactN(uint8_t *buf, uint8_t len, uint8_t row, uint8_t col,
                bool allowLetters) {
  while (true) {
    lcd.setCursor(col, row);
    for (uint8_t i = 0; i < len; i++) lcd.print(' ');
    int8_t n = getInputString(buf, len, row, col, 0, 0,
                              NO_TIMEOUT, true, allowLetters);
    if (n == (int8_t)len) return;
    lcd.setCursor(col, row);
    for (uint8_t i = 0; i < len; i++) lcd.print(' ');
  }
}

// ---------------- Enrollment (first boot, permanent) ----------------
// Entry is masked. Each value is typed twice; a mismatch restarts it,
// because there is no way to change it later.
void readConfirmedNumber(const char *label, uint8_t len,
                         uint16_t *out, bool isSeed, bool allowLetters) {
  uint8_t a[MAX_FIELD_LEN], b[MAX_FIELD_LEN];

  while (true) {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(label);
    readExactN(a, len, 1, 0, allowLetters);

    uint16_t v = isSeed ? digitsToSeed(a) : valuesToPin(a);
    if (isSeed && v == 0) {
      flashMessage("0000 NOT", "ALLOWED", 1800);
      continue;
    }

    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("CONFIRM:");
    readExactN(b, len, 1, 0, allowLetters);

    if (memcmp(a, b, len) == 0) { *out = v; return; }

    flashMessage("MISMATCH", "TRY AGAIN", 1800);
  }
}

void runEnrollment() {
  LOGLN("[setup] first boot — enrolling owner numbers");

  lcdWake();
  flashMessage("FIRST SETUP", "SET 2 CODES", 2500);

  // Wipe PIN: 4 chars, 0-9 or A-D (e.g. A12B)
  flashMessage("WIPE PIN: 4", "0-9 AND A-D OK", 2200);
  readConfirmedNumber("SET WIPE PIN:", PIN_LEN, &wipePin, false, true);

  // Restore number: 4 digits, no letters (must map cleanly to a seed)
  flashMessage("RESTORE: 4", "DIGITS ONLY", 2200);
  readConfirmedNumber("SET RESTORE:", CODE_LEN, &restoreSeed, true, false);

  saveOwner();
  ownerSet = true;

  seed = restoreSeed;
  saveSeed();

  LOG("[setup] wipePin=0x");  LOG(wipePin, HEX);
  LOG("  restore=0x");        LOGLN(restoreSeed, HEX);

  flashMessage("SETUP DONE", "KEEP BOTH SAFE", 3000);
  beepShort();
}

// Asks for the 6-digit Bluetooth pair key on the keypad. Masked, typed twice,
// digits only, may not start with 0. Runs on first setup and after a wipe.
void runPairKeySetup() {
  lcdWake();
  flashMessage("BLUETOOTH KEY", "6 DIGITS", 2200);

  while (true) {
    uint8_t a[PAIR_LEN], b[PAIR_LEN];

    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("SET PAIR KEY:");
    readExactN(a, PAIR_LEN, 1, 0, false);

    if (a[0] == 0) {
      flashMessage("CANNOT START", "WITH 0", 1800);
      continue;
    }

    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("CONFIRM:");
    readExactN(b, PAIR_LEN, 1, 0, false);

    if (memcmp(a, b, PAIR_LEN) != 0) {
      flashMessage("MISMATCH", "TRY AGAIN", 1800);
      continue;
    }

    uint32_t k = 0;
    for (uint8_t i = 0; i < PAIR_LEN; i++) k = k * 10 + a[i];
    pairKey    = k;
    pairKeySet = true;
    savePairKey();
    clearBondsOnStart = true;         // phones paired with an older key must re-pair
    break;
  }

  flashMessage("PAIR KEY SAVED", "KEEP IT SAFE", 2500);
  beepShort();
}

// ---------------- Validation ----------------
bool validateRollingCode(const uint8_t *buf, uint8_t n) {
  if (n < CODE_LEN) return false;
  uint8_t d[CODE_LEN];
  currentCodeDigits(d);
  for (uint8_t i = 0; i < CODE_LEN; i++) if (buf[i] != d[i]) return false;
  return true;
}

// ---------------- Outcomes ----------------
void clearLockoutState() {
  failCount        = 0;
  deadlockAttempts = 0;
  inDeadlock       = false;
  hardLockout      = false;
  challengeReady   = false;
  // postDeadlockPending is deliberately NOT touched here. runDeadlock()
  // sets it after clearing, and codeEntry()/handleFailure() clear it.
}

void unlockSystem() {
  // A correct rolling code was entered: clear everything, including the
  // post-deadlock flag.
  postDeadlockPending = false;
  clearLockoutState();

  lcdWake();
  lcd.setCursor(0, 0); lcd.print("OPEN");

  digitalWrite(LED_GREEN, HIGH);
  alertsLockBusy(true);              // [alert] no Wi-Fi bursts during the pulse
  solenoidFull();                    // pull the tongue in at full power
  delay(SOLENOID_PULL_MS);
  solenoidHold();                    // then hold it with a lower average current
  delay(SOLENOID_OPEN_MS - SOLENOID_PULL_MS);
  solenoidOff();                     // release: the lock closes
  alertsLockBusy(false);             // [alert]
  sendAlert(ALERT_UNLOCKED);         // [alert]
  digitalWrite(LED_GREEN, LOW);      // green LED mirrors the solenoid exactly

  lfsrUpdate();
  seed ^= (uint16_t)entropy;
  entropy = 0;
  guardSeed();
  saveSeed();
  blePublishCode();                  // send new code to the app over Bluetooth

  // No message after the unlock: the screen shows OPEN for the 3 s above and
  // then goes dark. The new code is only available in the companion app.
  logCode("[unlock]");

  standby();
}

void handleFailure(const char *why) {
  // Right after deadlock recovery the fail counter is not incremented for
  // the first attempt. The flag is cleared here, so only that one attempt
  // is protected; later wrong codes count normally.
  bool gracePeriod = postDeadlockPending;
  postDeadlockPending = false;
  sendAlert(ALERT_WRONG_CODE);       // [alert]

  if (!gracePeriod) {
    failCount++;
  }

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("ERROR");

  digitalWrite(LED_RED, HIGH);
  beepOnce();
  delay(1500);
  digitalWrite(LED_RED, LOW);

  LOG("[fail] ");
  LOG(why);
  if (gracePeriod) {
    LOG(" (grace — failCount unchanged)");
  }
  LOG(" — count=");
  LOGLN(failCount);

#if !DEBUG
  (void)why;
#endif

  if (hardLockout) {
    LOGLN("[fail] hard lockout active — valid code or wipe PIN only");
    standby();
    return;
  }

  if (!gracePeriod && failCount >= FAIL_LIMIT) {
    sendAlert(ALERT_LOCKOUT);        // [alert]
    inDeadlock       = true;
    deadlockAttempts = 0;
    challengeReady   = false;
  } else {
    standby();
  }
}

// Timeout is NOT a failed attempt — neither counter moves.
void handleTimeout(const char *where) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("TIMEOUT");
  lcd.setCursor(0, 1); lcd.print("INPUT CLEARED");
  timeoutTone();

  LOG("[timeout] during ");
  LOG(where);
  LOG(" — fails still ");
  LOGLN(failCount);

#if !DEBUG
  (void)where;
#endif

  delay(1500);
}

void enterHardLockout() {
  sendAlert(ALERT_HARD_LOCKOUT);      // [alert]
  hardLockout         = true;
  inDeadlock          = false;
  challengeReady      = false;
  postDeadlockPending = false;       // can't be in recovery if locking out

  LOGLN();
  LOGLN("*** WARNING ***");
  LOG  ("*** Deadlock recovery exhausted: ");
  LOG  (DEADLOCK_LIMIT);
  LOG  ("/");
  LOG  (DEADLOCK_LIMIT);
  LOGLN(" attempts failed");
  LOGLN("*** Recovery is no longer offered.");
  LOGLN("*** Owner must enter a valid rolling code, or '*' + wipe PIN.");
  LOGLN();

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("LOCKED OUT");
  lcd.setCursor(0, 1); lcd.print("CODE OR WIPE");

  digitalWrite(LED_RED, HIGH);
  beepOnce();
  delay(2500);
  digitalWrite(LED_RED, LOW);

  standby();
}

// ---------------- Deadlock ----------------
// Two RANDOM digits; user types the 9's complement of each, then '#'.
// Capped at DEADLOCK_LIMIT attempts. Timeouts do not consume an attempt.
//
// On SUCCESS: counters are cleared, postDeadlockPending is set, a prompt
// is shown, then the system goes dark. The lock stays CLOSED; the user
// must press '#' and enter the current rolling code to open it.
void runDeadlock() {
  if (!challengeReady) newChallenge();

  lcdWake();
  lcd.setCursor(0, 0);
  lcd.print("LOCK:");
  lcd.print((char)('0' + challengeA));
  lcd.print((char)('0' + challengeB));
  lcd.setCursor(11, 0);
  lcd.print(deadlockAttempts + 1);
  lcd.print("/");
  lcd.print(DEADLOCK_LIMIT);

  LOG("[deadlock] attempt ");
  LOG(deadlockAttempts + 1);
  LOG("/");
  LOG(DEADLOCK_LIMIT);
  LOG("  challenge=");
  LOG(challengeA); LOG(challengeB);
  LOG("  expected=");
  LOG(9 - challengeA); LOGLN(9 - challengeB);

  uint8_t buf[MAX_FIELD_LEN] = {0};
  int8_t  n = getInputString(buf, CODE_LEN, 1, 0, 1, 13,
                             INPUT_TIMEOUT_MS, true, false);

  if (n == INPUT_TIMED_OUT) {
    handleTimeout("deadlock");
    challengeReady = false;          // fresh challenge, attempt not consumed
    return;
  }

  bool ok = (n >= 2) &&
            (buf[0] == (uint8_t)(9 - challengeA)) &&
            (buf[1] == (uint8_t)(9 - challengeB));

  if (ok) {
    // Deadlock escaped. No solenoid, no green LED: the lock stays closed.
    clearLockoutState();
    postDeadlockPending = true;

    LOGLN("[deadlock] recovery accepted — user must now enter rolling code");
    logCode("[deadlock-recovered]");

    // Neutral two-beep acknowledgement only
    beepShort();
    delay(200);
    beepShort();

    lcdWake();
    lcd.setCursor(0, 0); lcd.print("PRESS # + CODE");
    delay(2500);

    standby();                       // loop() picks up '#' for codeEntry()
    return;
  }

  // Wrong response
  deadlockAttempts++;
  LOG("[deadlock] rejected — attempts used ");
  LOG(deadlockAttempts);
  LOG("/");
  LOGLN(DEADLOCK_LIMIT);

  if (deadlockAttempts >= DEADLOCK_LIMIT) {
    enterHardLockout();
    return;
  }

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("?");
  digitalWrite(LED_RED, HIGH);
  beepOnce();
  delay(1200);
  digitalWrite(LED_RED, LOW);
  challengeReady = false;
}

// ---------------- Gated factory wipe ----------------
// '*' followed by the 4-character owner PIN (0-9 and A-D). On success the
// rolling seed is reset to the restore number and the lock returns to
// standby — no halt, no power cycle. '#' then opens with the restore number.
void systemWipe() {
  sendAlert(ALERT_WIPE_ATTEMPT);      // [alert]
  lcdWake();
  lcd.setCursor(0, 0); lcd.print("WIPE PIN:");

  uint8_t buf[MAX_FIELD_LEN] = {0};
  int8_t  n = getInputString(buf, PIN_LEN, 1, 0, 1, 13,
                             INPUT_TIMEOUT_MS, true, true);

  if (n == INPUT_TIMED_OUT) {
    handleTimeout("wipe pin");
    standby();
    return;
  }

  if (n < PIN_LEN || valuesToPin(buf) != wipePin) {
    handleFailure("wrong wipe PIN");  // counts toward the deadlock
    return;
  }

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("WIPE");

  resetLockToRestore();
  clearLockoutState();
  postDeadlockPending = false;        // wipe is a full reset

  LOG("[wipe] accepted — seed reset to 0x");
  LOGLN(seed, HEX);
  logCode("[wipe]");

  solenoidOff();                      // deliberately left de-energized
  digitalWrite(LED_GREEN, HIGH);
  wipeTone();

  lcd.setCursor(0, 1); lcd.print("USE RESTORE NO");
  delay(3000);

  // Forget every paired phone and erase the pair key. The lock then restarts
  // and asks for a NEW pair key on the keypad (see setup()).
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("CLEARING BT...");
  lcd.setCursor(0, 1); lcd.print("NEW KEY NEXT");
  bleClearBonds();
  clearPairKey();
  LOGLN("[wipe] paired phones forgotten, pair key erased, restarting");
  delay(4000);                        // give the stack time to save to flash

  digitalWrite(LED_GREEN, LOW);
  alertsMarkWipeDone();               // [alert] sent after the reboot
  ESP.restart();                      // setup() asks for the new pair key
}

// ---------------- Code entry ----------------
// Used for normal entry AND the mandatory open after deadlock recovery.
// The code must match in both cases. After recovery, the first wrong
// entry gets a grace pass (see handleFailure).
void codeEntry() {
  lcdWake();

  uint8_t buf[MAX_FIELD_LEN] = {0};
  int8_t  n;

  if (postDeadlockPending) {
    // Digits go on row 1, cols 0-3. The countdown sits at row 1, col 13
    // so it never overwrites the digits being typed.
    lcd.setCursor(0, 0); lcd.print("CODE (ESCAPED):");
    n = getInputString(buf, CODE_LEN, 1, 0, 1, 13,
                       INPUT_TIMEOUT_MS, true, false);
  } else {
    lcd.setCursor(0, 0); lcd.print("CODE:");
    n = getInputString(buf, CODE_LEN, 0, 5, 1, 0,
                       INPUT_TIMEOUT_MS, true, false);
  }

  if (n == INPUT_TIMED_OUT) {
    // Flag stays set after a timeout so the hint and grace pass remain.
    handleTimeout("code entry");
    standby();
    return;
  }

  if (validateRollingCode(buf, (uint8_t)n)) unlockSystem();
  else                                      handleFailure("wrong code");
}

// ---------------- Bluetooth (BLE) ----------------
#define BLE_SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_CHAR_UUID    "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define BLE_WINDOW_MS    60000UL      // visible for 60 s after each unlock
#define BLE_ALWAYS_ON    1            // 1 = always visible (paired phones only)
                                      // 0 = only for BLE_WINDOW_MS after unlock

// NimBLE (not Bluedroid): same name, UUIDs, static 6-digit passkey,
// bonding + MITM + secure connections. Uses ~100 KB less RAM, which the
// push-alert HTTPS connection needs. Bonds are kept in NVS by NimBLE.
NimBLEServer         *bleServer;
NimBLECharacteristic *bleChar;
bool     bleAdvertising = false;
uint32_t bleStopAt      = 0;

class LockServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *, NimBLEConnInfo &) override { logHeap("phone connected"); }   // [alert] memory check

  // Restart advertising after a phone disconnects.
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override {
    if (bleAdvertising) NimBLEDevice::startAdvertising();
  }

  // Drop a phone whose pairing did not end encrypted + authenticated (wrong key).
  void onAuthenticationComplete(NimBLEConnInfo &info) override {
    if (!info.isEncrypted() || !info.isAuthenticated()) {
      LOGLN("[ble] pairing failed, phone disconnected");
      bleServer->disconnect(info.getConnHandle());
    } else {
      LOGLN("[ble] phone paired");
    }
  }
};

// Removes every bonded (paired) phone from the lock.
void bleClearBonds() {
  NimBLEDevice::deleteAllBonds();
}

void bleBegin() {
  NimBLEDevice::init("RollingLock");

  if (clearBondsOnStart) {            // new pair key => forget all old pairings
    bleClearBonds();
    clearBondsOnStart = false;
  }

  // ---- Security: require pairing with a static 6-digit passkey ----
  NimBLEDevice::setSecurityAuth(true, true, true);       // bonding, MITM, secure connection
  NimBLEDevice::setSecurityPasskey(pairKey);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY); // phone types the passkey

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new LockServerCb());
  bleServer->advertiseOnDisconnect(false);               // handled in onDisconnect()
  NimBLEService *svc = bleServer->createService(BLE_SERVICE_UUID);

  // Reading needs an encrypted, passkey-authenticated (bonded) link.
  bleChar = svc->createCharacteristic(
      BLE_CHAR_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN |
      NIMBLE_PROPERTY::NOTIFY);
  bleChar->setValue("----");          // nothing revealed until an unlock
  svc->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SERVICE_UUID);
  adv->enableScanResponse(true);      // name goes in the scan response (adv packet is full)
  adv->setName("RollingLock");
}

void blePublishCode() {
  uint8_t d[CODE_LEN];
  currentCodeDigits(d);
  char s[CODE_LEN + 1];
  for (uint8_t i = 0; i < CODE_LEN; i++) s[i] = '0' + d[i];
  s[CODE_LEN] = 0;

  bleChar->setValue(s);
  // Notify only phones on an encrypted + authenticated (paired) link.
  for (uint16_t h : bleServer->getPeerDevices()) {
    NimBLEConnInfo ci = bleServer->getPeerInfoByHandle(h);
    if (ci.isEncrypted() && ci.isAuthenticated()) bleChar->notify(h);
  }

  if (!bleAdvertising) {
    NimBLEDevice::startAdvertising();
    bleAdvertising = true;
  }
  bleStopAt = millis() + BLE_WINDOW_MS;
  LOGLN("[ble] code published");
}

void bleService() {                   // call from loop()
  if (!BLE_ALWAYS_ON && bleAdvertising && (int32_t)(millis() - bleStopAt) >= 0) {
    NimBLEDevice::stopAdvertising();
    bleAdvertising = false;
    LOGLN("[ble] advertising window closed");
  }
}

// ---------------- Setup / loop ----------------
void setup() {
#if DEBUG
  Serial.begin(115200);
#endif
  logHeap("boot");                   // [alert] memory check
  alertsBegin();                      // [alert] start the sender task

  solenoidInit();                     // PWM pin, starts switched off
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_GREEN, LOW);

  Wire.begin(21, 22);                 // SDA, SCL
  lcd.init();
  lcd.backlight();

  LOGLN("=== Rolling Code Lock: Stage 7 ===");

  loadOwner();
  if (!ownerSet) runEnrollment();
  else           loadSeed();

  loadPairKey();
  if (!pairKeySet) runPairKeySetup(); // first setup, or right after a wipe

  bleBegin();                         // uses the pair key
  logHeap("ble up");                 // [alert] memory check

  logCode("[boot]");
  blePublishCode();                   // current code available right after boot

  beepShort();
  standby();                          // start dark
}

void loop() {
  bleService();

  if (inDeadlock) {
    runDeadlock();
    return;
  }

  char key = keypad.getKey();
  if (!key) return;

  injectEntropy();
  beepShort();

  if (key == '#')      codeEntry();
  else if (key == '*') systemWipe();
}
