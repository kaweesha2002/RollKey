/*
 * ESP32 Rolling Code Lock — Stage 7 (final, with post-deadlock flow)
 * Enrollment / dark standby / code entry / unlock / deadlock / gated wipe
 *
 * Board: ESP32 Dev Module (30-pin DevKit, WROOM)
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
 * Deviations from the AVR source (all deliberate):
 *   - seed == 0 is clamped to the restore number (AVR trapped there forever)
 *   - explicit "init" flag in NVS replaces the 0xFFFF blank sentinel
 *   - factory wipe fires LED + buzzer only; the relay stays de-energized
 *   - deadlock challenge digits come from the hardware RNG, not seed_L
 *   - 10s inter-key timeout; never counts as a failed attempt
 *   - wipe is gated by the owner PIN; wrong PIN counts toward the deadlock
 *   - deadlock recovery is capped at 2 attempts, then hard lockout
 *   - wipe resets the code to the restore number, forgets all paired phones,
 *     erases the Bluetooth pair key and restarts; the lock then asks for a
 *     new pair key on the keypad
 *   - LCD sleeps (display + backlight off) in standby, wakes on '#' or '*'
 *   - solenoid is energised for SOLENOID_OPEN_MS (3 s) only, then released; the LCD shows OPEN for
 *     that time and then goes dark
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
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <Preferences.h>
#include <esp_system.h>
#include <string.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#include <esp_gap_ble_api.h>

// ---------------- Debug switch ----------------
#define DEBUG 1          // 1 = testing, 0 = final build

#if DEBUG
  #define LOG(...)   Serial.print(__VA_ARGS__)
  #define LOGLN(...) Serial.println(__VA_ARGS__)
#else
  #define LOG(...)
  #define LOGLN(...)
#endif

// ---------------- Pin map ----------------
#define LED_RED   23      // PC4: error indicator
#define LED_GREEN 19      // PC5: unlock / wipe indicator
#define BUZZER    18      // PD2
#define RELAY     4       // solenoid driver pin: GPIO4 -> 220 ohm -> IRLZ44N gate (10k gate->GND pull-down)
#define RELAY_ACTIVE_LOW 0   // 0 = HIGH switches the solenoid ON (MOSFET, or active-high relay)
                             // 1 = LOW switches it ON (active-low relay module only)

// Relay control.
// Active-low modules powered from 5V often stay ON when the 3.3V ESP32 drives
// the pin HIGH. So for those modules, "off" RELEASES the pin (high impedance)
// and the module's own pull-up turns the relay off; "on" drives it LOW.
void relayOn() {
#if RELAY_ACTIVE_LOW
  digitalWrite(RELAY, LOW);
  pinMode(RELAY, OUTPUT);
#else
  digitalWrite(RELAY, HIGH);
  pinMode(RELAY, OUTPUT);
#endif
}

void relayOff() {
#if RELAY_ACTIVE_LOW
  pinMode(RELAY, INPUT);             // release the pin
#else
  digitalWrite(RELAY, LOW);
  pinMode(RELAY, OUTPUT);
#endif
}
#define SOLENOID_OPEN_MS 3000  // how long the lock stays open after a correct code

LiquidCrystal_I2C lcd(0x27, 16, 2);   // change to 0x3F if your scanner says so
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
void beepOnce() {
  tone(BUZZER, 400);
  delay(500);
  noTone(BUZZER);
}

void beepShort() {
  tone(BUZZER, 1200);
  delay(40);
  noTone(BUZZER);
}

// Sounded when a key is not valid in the current field
void beepReject() {
  tone(BUZZER, 200);
  delay(90);
  noTone(BUZZER);
}

void timeoutTone() {
  tone(BUZZER, 600); delay(80);  noTone(BUZZER);
  delay(60);
  tone(BUZZER, 450); delay(120); noTone(BUZZER);
}

void wipeTone() {
  for (uint8_t i = 0; i < 3; i++) {
    tone(BUZZER, 800); delay(150); noTone(BUZZER);
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
  relayOn();                         // lock opens
  delay(SOLENOID_OPEN_MS);           // stays open for 3 s ...
  relayOff();                        // ... then the solenoid releases and the lock closes
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

  relayOff();                         // deliberately left de-energized
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

BLECharacteristic *bleChar;
bool     bleAdvertising = false;
uint32_t bleStopAt      = 0;

// Restart advertising after a phone disconnects (the stack stops it).
class LockServerCb : public BLEServerCallbacks {
  void onDisconnect(BLEServer *) override {
    if (bleAdvertising) {
      delay(300);
      BLEDevice::startAdvertising();
    }
  }
};

// Removes every bonded (paired) phone from the lock.
void bleClearBonds() {
  int n = esp_ble_get_bond_device_num();
  if (n <= 0) return;
  esp_ble_bond_dev_t *list = (esp_ble_bond_dev_t *)malloc(sizeof(esp_ble_bond_dev_t) * n);
  if (!list) return;
  if (esp_ble_get_bond_device_list(&n, list) == ESP_OK) {
    for (int i = 0; i < n; i++) esp_ble_remove_bond_device(list[i].bd_addr);
  }
  free(list);
}

void bleBegin() {
  BLEDevice::init("RollingLock");

  if (clearBondsOnStart) {            // new pair key => forget all old pairings
    bleClearBonds();
    clearBondsOnStart = false;
  }

  // ---- Security: require pairing with a static 6-digit passkey ----
  // (API of ESP32 Arduino core 3.x)
  BLESecurity *sec = new BLESecurity();
  sec->setPassKey(true, pairKey);              // true = static passkey
  sec->setCapability(ESP_IO_CAP_OUT);          // phone types the passkey
  sec->setAuthenticationMode(true, true, true);// bonding, MITM, secure connection

  BLEServer  *server = BLEDevice::createServer();
  server->setCallbacks(new LockServerCb());
  BLEService *svc    = server->createService(BLE_SERVICE_UUID);

  bleChar = svc->createCharacteristic(
      BLE_CHAR_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  bleChar->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM);

  BLE2902 *cccd = new BLE2902();
  cccd->setAccessPermissions((esp_gatt_perm_t)(ESP_GATT_PERM_READ_ENC_MITM |
                                               ESP_GATT_PERM_WRITE_ENC_MITM));
  bleChar->addDescriptor(cccd);
  bleChar->setValue("----");          // nothing revealed until an unlock
  svc->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SERVICE_UUID);
  adv->setScanResponse(true);
}

void blePublishCode() {
  uint8_t d[CODE_LEN];
  currentCodeDigits(d);
  char s[CODE_LEN + 1];
  for (uint8_t i = 0; i < CODE_LEN; i++) s[i] = '0' + d[i];
  s[CODE_LEN] = 0;

  bleChar->setValue(s);
  bleChar->notify();

  if (!bleAdvertising) {
    BLEDevice::startAdvertising();
    bleAdvertising = true;
  }
  bleStopAt = millis() + BLE_WINDOW_MS;
  LOGLN("[ble] code published");
}

void bleService() {                   // call from loop()
  if (!BLE_ALWAYS_ON && bleAdvertising && (int32_t)(millis() - bleStopAt) >= 0) {
    BLEDevice::stopAdvertising();
    bleAdvertising = false;
    LOGLN("[ble] advertising window closed");
  }
}

// ---------------- Setup / loop ----------------
void setup() {
#if DEBUG
  Serial.begin(115200);
#endif

  relayOff();                         // relay off as early as possible
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
