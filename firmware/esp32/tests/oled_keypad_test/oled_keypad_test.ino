// OLED + keypad test. The ESP32 is powered from the laptop over USB.
// OLED:   VDD -> 3V3, GND -> GND, SCK -> GPIO22, SDA -> GPIO21
// Keypad: rows GPIO13, 14, 27, 26   columns GPIO25, 33, 32, 16
// Keep the VIN pin disconnected while USB is plugged in.
// Open the Serial Monitor at 115200 baud.
#include <Wire.h>
#include <U8g2lib.h>
#include <Keypad.h>
#include <string.h>

#define OLED_SH1106 1     // 1 = SH1106 (most 1.3 inch), 0 = SSD1306 (0.96 inch)
#define OLED_FLIP     1       // 1 = screen mounted upside down (rotate 180), 0 = normal
#define OLED_ROTATION (OLED_FLIP ? U8G2_R2 : U8G2_R0)
#define SDA_PIN     21
#define SCL_PIN     22
#define SKIP_PIN    0     // BOOT button: skip a key that does not respond

#if OLED_SH1106
  U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(OLED_ROTATION, U8X8_PIN_NONE);
#else
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(OLED_ROTATION, U8X8_PIN_NONE);
#endif

const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {13, 14, 27, 26};
byte colPins[COLS] = {25, 33, 32, 16};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

enum State { GUIDED, RESULT, FREE };
State   state = GUIDED;
char    order[16];          // guided order: row by row
bool    ok[16];
uint8_t idx = 0;            // key currently being tested
char    lastGot = '-';
char    history[17] = "";
uint8_t oledAddr = 0;

bool found(uint8_t a) {
  Wire.beginTransmission(a);
  return Wire.endTransmission() == 0;
}

bool findKey(char k, int &row, int &col) {
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++)
      if (keys[r][c] == k) { row = r + 1; col = c + 1; return true; }
  return false;
}

int countBad() {
  int n = 0;
  for (uint8_t i = 0; i < 16; i++) if (!ok[i]) n++;
  return n;
}

void drawGuided() {
  if (!oledAddr) return;
  char line[24];
  char e[2] = { order[idx], 0 };
  char g[2] = { lastGot, 0 };
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  snprintf(line, sizeof(line), "Press key %u of 16", idx + 1);
  u8g2.drawStr(0, 12, line);
  u8g2.setFont(u8g2_font_logisoso32_tf);
  u8g2.drawStr(12, 58, e);
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(66, 34, "Got:");
  u8g2.setFont(u8g2_font_logisoso20_tf);
  u8g2.drawStr(72, 58, g);
  u8g2.sendBuffer();
}

void drawResult() {
  if (!oledAddr) return;
  char bad[16] = "";
  int n = 0;
  for (uint8_t i = 0; i < 16; i++)
    if (!ok[i] && n < 14) { bad[n++] = order[i]; bad[n++] = ' '; }
  bad[n] = 0;
  int nBad = countBad();
  char line[24];
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_8x13B_tf);
  u8g2.drawStr(0, 14, nBad ? "KEYPAD FAIL" : "KEYPAD PASS");
  u8g2.setFont(u8g2_font_6x12_tf);
  if (nBad) {
    snprintf(line, sizeof(line), "Bad: %s", bad);
    u8g2.drawStr(0, 32, line);
    snprintf(line, sizeof(line), "%d of 16 keys failed", nBad);
    u8g2.drawStr(0, 46, line);
  } else {
    u8g2.drawStr(0, 32, "All 16 keys OK");
  }
  u8g2.drawStr(0, 62, "Press a key: free mode");
  u8g2.sendBuffer();
}

void drawFree(char k, int r, int c) {
  if (!oledAddr) return;
  char line[24];
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_8x13B_tf);
  u8g2.drawStr(0, 14, history);
  u8g2.setFont(u8g2_font_6x12_tf);
  snprintf(line, sizeof(line), "Last: %c  Row %d Col %d", k, r, c);
  u8g2.drawStr(0, 40, line);
  u8g2.drawStr(0, 62, "Free mode");
  u8g2.sendBuffer();
}

void addHistory(char k) {
  size_t n = strlen(history);
  if (n >= 16) { memmove(history, history + 1, 15); n = 15; }
  history[n] = k;
  history[n + 1] = 0;
}

void nextKey() {
  idx++;
  if (idx >= 16) {
    state = RESULT;
    int nBad = countBad();
    if (nBad == 0) {
      Serial.println("RESULT: PASS, all 16 keys OK");
    } else {
      Serial.printf("RESULT: FAIL, %d keys not confirmed:", nBad);
      for (uint8_t i = 0; i < 16; i++) if (!ok[i]) Serial.printf(" %c", order[i]);
      Serial.println();
    }
    Serial.println("Press any key for free mode");
    drawResult();
  }
}

void handleKey(char k) {
  int r = 0, c = 0;
  findKey(k, r, c);

  if (state == GUIDED) {
    lastGot = k;
    if (k == order[idx]) {
      ok[idx] = true;
      Serial.printf("OK     expected %c, got %c (row %d, col %d)\n", order[idx], k, r, c);
      nextKey();
    } else {
      Serial.printf("WRONG  expected %c, got %c (map row %d, col %d)\n", order[idx], k, r, c);
    }
    if (state == GUIDED) drawGuided();
    return;
  }

  if (state == RESULT) { state = FREE; history[0] = 0; }
  addHistory(k);
  Serial.printf("Key %c (row %d, col %d)\n", k, r, c);
  drawFree(k, r, c);
}

void setup() {
  Serial.begin(115200);
  pinMode(SKIP_PIN, INPUT_PULLUP);
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) { order[r * COLS + c] = keys[r][c]; ok[r * COLS + c] = false; }

  Wire.begin(SDA_PIN, SCL_PIN);
  if (found(0x3C))      oledAddr = 0x3C;
  else if (found(0x3D)) oledAddr = 0x3D;

  Serial.println();
  if (oledAddr) {
    Serial.printf("OLED found at 0x%02X\n", oledAddr);
    u8g2.setI2CAddress(oledAddr << 1);
    u8g2.begin();
    u8g2.setBusClock(400000);
  } else {
    Serial.println("OLED NOT found: check the wiring and the 3V3 connection (keypad test still runs)");
  }

  Serial.println("Keypad test: press the key shown on the screen, in order:");
  Serial.println("  1 2 3 A / 4 5 6 B / 7 8 9 C / * 0 # D");
  Serial.println("Press the BOOT button to skip a key that does not respond");
  drawGuided();
}

void loop() {
  if (state == GUIDED && digitalRead(SKIP_PIN) == LOW) {
    delay(30);
    if (digitalRead(SKIP_PIN) == LOW) {
      Serial.printf("SKIPPED key %c\n", order[idx]);
      ok[idx] = false;
      nextKey();
      if (state == GUIDED) drawGuided();
      while (digitalRead(SKIP_PIN) == LOW) delay(10);
    }
  }

  char k = keypad.getKey();
  if (k) handleKey(k);
}
