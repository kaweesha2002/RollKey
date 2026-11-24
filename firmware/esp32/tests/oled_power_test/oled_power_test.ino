// OLED test for the new power setup (no laptop needed after upload)
// OLED: VDD -> 3V3, GND -> GND, SCK -> GPIO22, SDA -> GPIO21
// Power the ESP32 on VIN from the 5V rail, USB unplugged.
#include <Wire.h>
#include <U8g2lib.h>
#include <Preferences.h>
#include <esp_system.h>

#define OLED_SH1106  1        // 1 = SH1106 (most 1.3 inch), 0 = SSD1306 (0.96 inch)
#define OLED_FLIP     1       // 1 = screen mounted upside down (rotate 180), 0 = normal
#define OLED_ROTATION (OLED_FLIP ? U8G2_R2 : U8G2_R0)
#define FIRE_LOCK    0        // 1 = also pulse the lock (GPIO4) for 1 s every 8 s
#define SDA_PIN      21
#define SCL_PIN      22
#define SOLENOID_PIN 4
#define STATUS_LED   2        // blue LED on most ESP32 boards

#if OLED_SH1106
  U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(OLED_ROTATION, U8X8_PIN_NONE);
#else
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(OLED_ROTATION, U8X8_PIN_NONE);
#endif

Preferences prefs;
uint8_t     oledAddr  = 0;
uint32_t    boots     = 0;
const char *resetText = "?";

const char *reasonText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_SW:        return "SOFTWARE";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "WATCHDOG";
    case ESP_RST_TASK_WDT:  return "WATCHDOG";
    case ESP_RST_WDT:       return "WATCHDOG";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_EXT:       return "EXT RESET";
    default:                return "OTHER";
  }
}

bool found(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void drawInfo(uint32_t sec) {
  char line[24];
  u8g2.setFont(u8g2_font_8x13B_tf);
  u8g2.drawStr(0, 13, "OLED TEST");
  u8g2.setFont(u8g2_font_6x12_tf);
  snprintf(line, sizeof(line), "Addr 0x%02X  %s", oledAddr, OLED_SH1106 ? "SH1106" : "SSD1306");
  u8g2.drawStr(0, 28, line);
  snprintf(line, sizeof(line), "Reset: %s", resetText);
  u8g2.drawStr(0, 41, line);
  snprintf(line, sizeof(line), "Boots:%lu Up:%lus", (unsigned long)boots, (unsigned long)sec);
  u8g2.drawStr(0, 54, line);
}

void drawEdges() {
  u8g2.drawFrame(0, 0, 128, 64);
  u8g2.drawLine(0, 0, 127, 63);
  u8g2.drawLine(0, 63, 127, 0);
  u8g2.setDrawColor(0);
  u8g2.drawBox(36, 24, 56, 16);
  u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(40, 36, "EDGES");
}

void drawBar(int w) {
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 14, "Moving bar");
  u8g2.drawFrame(2, 28, 124, 16);
  u8g2.drawBox(4, 30, w > 120 ? 120 : w, 12);
}

void setup() {
  Serial.begin(115200);
  pinMode(SOLENOID_PIN, OUTPUT);
  digitalWrite(SOLENOID_PIN, LOW);       // lock stays off unless FIRE_LOCK is 1
  pinMode(STATUS_LED, OUTPUT);

  esp_reset_reason_t r = esp_reset_reason();
  resetText = reasonText(r);

  prefs.begin("oledtest", false);
  boots = (r == ESP_RST_POWERON) ? 1 : prefs.getUInt("boots", 0) + 1;
  prefs.putUInt("boots", boots);
  prefs.end();

  Wire.begin(SDA_PIN, SCL_PIN);
  if (found(0x3C))      oledAddr = 0x3C;
  else if (found(0x3D)) oledAddr = 0x3D;

  Serial.printf("Reset reason: %s, boots: %lu\n", resetText, (unsigned long)boots);
  if (oledAddr) {
    Serial.printf("OLED found at 0x%02X\n", oledAddr);
    u8g2.setI2CAddress(oledAddr << 1);
    u8g2.begin();
    u8g2.setBusClock(400000);
  } else {
    Serial.println("OLED NOT found: check wiring and power");
  }
}

void loop() {
  uint32_t now = millis();

  // Blue LED: slow heartbeat = OLED found, fast blink = OLED not found
  uint32_t period = oledAddr ? 1000 : 200;
  digitalWrite(STATUS_LED, (now % period) < (period / 2));

#if FIRE_LOCK
  static uint32_t lastPulse = 0;
  static bool lockOn = false;
  if (!lockOn && now - lastPulse >= 8000) {
    digitalWrite(SOLENOID_PIN, HIGH);
    lockOn = true;
    lastPulse = now;
  } else if (lockOn && now - lastPulse >= 1000) {
    digitalWrite(SOLENOID_PIN, LOW);
    lockOn = false;
  }
#endif

  if (!oledAddr) return;

  uint32_t t = now % 12000;               // 12 s cycle
  u8g2.clearBuffer();
  if (t < 4000)       drawInfo(now / 1000);
  else if (t < 6000)  u8g2.drawBox(0, 0, 128, 64);
  else if (t < 8000)  drawEdges();
  else                drawBar((int)((t - 8000) * 120 / 4000));
  u8g2.sendBuffer();
  delay(50);
}
