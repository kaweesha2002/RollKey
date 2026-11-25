// All-components test (no keypad needed). Loops through every output so you
// can watch / listen to each one, and shows the current step on the OLED.
//
// Wiring (same as the main lock sketch):
//   OLED      SDA GPIO21, SCL GPIO22, VDD 3V3
//   Red LED   GPIO23 (220 ohm)      Green LED GPIO19 (220 ohm)
//   Buzzer    GPIO18
//   Solenoid  GPIO4 -> 220 ohm -> IRLZ44N gate (10k gate -> GND), lock on 9 V boost
//
// Cycle (about 14 s), repeats forever:
//   1. OLED pattern (white screen, frame + X, text)    3 s
//   2. Red LED on                                       1.5 s
//   3. Green LED on                                     1.5 s
//   4. Buzzer: low / mid / high beeps                   ~1.5 s
//   5. Solenoid: pull-in then hold, open 3 s            3 s
//   6. Pause                                            ~2 s
// Serial Monitor 115200 prints each step. Send 's' to skip the solenoid
// step (toggle) if the clicking gets annoying.

#include <Wire.h>
#include <U8g2lib.h>

#define OLED_SH1106   1       // 1 = SH1106 (1.3 inch), 0 = SSD1306
#define OLED_FLIP     1       // 1 = screen mounted upside down (rotate 180), 0 = normal
#define OLED_ROTATION (OLED_FLIP ? U8G2_R2 : U8G2_R0)
#define SDA_PIN       21
#define SCL_PIN       22

#define LED_RED       23
#define LED_GREEN     19
#define BUZZER        18
#define SOLENOID_PIN  4
#define STATUS_LED    2

#define SOLENOID_PULL_MS    300
#define SOLENOID_HOLD_DUTY  100
#define SOLENOID_OPEN_MS    3000
#define SOLENOID_PWM_HZ     20000

#if OLED_SH1106
  U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(OLED_ROTATION, U8X8_PIN_NONE);
#else
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(OLED_ROTATION, U8X8_PIN_NONE);
#endif

uint8_t  oledAddr = 0;
uint32_t cycle    = 0;
bool     skipLock = false;

bool found(uint8_t a) {
  Wire.beginTransmission(a);
  return Wire.endTransmission() == 0;
}

void show(const char *title, const char *line2) {
  if (!oledAddr) return;
  char top[22];
  snprintf(top, sizeof(top), "TEST  cycle %lu", (unsigned long)cycle);
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 10, top);
  u8g2.drawHLine(0, 13, 128);
  u8g2.setFont(u8g2_font_10x20_tf);
  u8g2.drawStr(0, 38, title);
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 58, line2);
  u8g2.sendBuffer();
}

void step(const char *title, const char *line2) {
  Serial.printf("[%lu] %s %s\n", (unsigned long)cycle, title, line2);
  show(title, line2);
}

void checkSerial() {
  while (Serial.available()) {
    if (Serial.read() == 's') {
      skipLock = !skipLock;
      Serial.println(skipLock ? "solenoid step SKIPPED" : "solenoid step ON");
    }
  }
}

void testOled() {
  if (!oledAddr) { Serial.println("OLED: NOT found, skipping"); return; }
  Serial.printf("[%lu] OLED patterns\n", (unsigned long)cycle);
  u8g2.clearBuffer(); u8g2.drawBox(0, 0, 128, 64); u8g2.sendBuffer();
  delay(1000);
  u8g2.clearBuffer();
  u8g2.drawFrame(0, 0, 128, 64);
  u8g2.drawLine(0, 0, 127, 63);
  u8g2.drawLine(0, 63, 127, 0);
  u8g2.sendBuffer();
  delay(1000);
  step("OLED", "Text upright?");
  delay(1000);
}

void testLed(int pin, const char *name) {
  step(name, "LED should be ON");
  digitalWrite(pin, HIGH);
  delay(1500);
  digitalWrite(pin, LOW);
}

void testBuzzer() {
  // Four modes, 0.8 s each, so you can hear which one is loudest:
  //   1 = old tone 800 Hz, 2 = 2700 Hz (typical piezo resonance),
  //   3 = 4000 Hz, 4 = pin held HIGH (an active buzzer beeps by itself)
  const char *label[] = {"1: 800 Hz (old)", "2: 2700 Hz", "3: 4000 Hz", "4: DC on (active)"};
  int freq[] = {800, 2700, 4000, 0};
  for (int i = 0; i < 4; i++) {
    step("BUZZER", label[i]);
    if (freq[i]) tone(BUZZER, freq[i]);
    else { pinMode(BUZZER, OUTPUT); digitalWrite(BUZZER, HIGH); }
    delay(800);
    noTone(BUZZER);
    digitalWrite(BUZZER, LOW);
    delay(400);
  }
}

void testSolenoid() {
  if (skipLock) { step("LOCK", "skipped ('s' to enable)"); delay(1000); return; }
  step("LOCK", "should click open 3s");
  ledcWrite(SOLENOID_PIN, 255);
  digitalWrite(STATUS_LED, HIGH);
  delay(SOLENOID_PULL_MS);
  ledcWrite(SOLENOID_PIN, SOLENOID_HOLD_DUTY);
  delay(SOLENOID_OPEN_MS - SOLENOID_PULL_MS);
  ledcWrite(SOLENOID_PIN, 0);
  digitalWrite(STATUS_LED, LOW);
  Serial.println("      lock released");
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_RED, OUTPUT);    digitalWrite(LED_RED, LOW);
  pinMode(LED_GREEN, OUTPUT);  digitalWrite(LED_GREEN, LOW);
  pinMode(STATUS_LED, OUTPUT); digitalWrite(STATUS_LED, LOW);
  ledcAttach(SOLENOID_PIN, SOLENOID_PWM_HZ, 8);
  ledcWrite(SOLENOID_PIN, 0);

  Wire.begin(SDA_PIN, SCL_PIN);
  if (found(0x3C))      oledAddr = 0x3C;
  else if (found(0x3D)) oledAddr = 0x3D;
  delay(300);
  Serial.println();
  if (oledAddr) {
    Serial.printf("OLED found at 0x%02X (flip %d)\n", oledAddr, OLED_FLIP);
    u8g2.setI2CAddress(oledAddr << 1);
    u8g2.begin();
  } else {
    Serial.println("OLED NOT found: check wiring");
  }
  Serial.println("All-components test. Send 's' to skip/enable the solenoid step.");
}

void loop() {
  cycle++;
  checkSerial(); testOled();
  checkSerial(); testLed(LED_RED, "RED LED");
  checkSerial(); testLed(LED_GREEN, "GREEN LED");
  checkSerial(); testBuzzer();
  checkSerial(); testSolenoid();
  step("DONE", "next cycle in 2 s");
  delay(2000);
}
