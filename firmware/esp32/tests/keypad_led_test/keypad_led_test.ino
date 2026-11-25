// Keypad + LED test, using the SAME pin settings as the lock firmware.
// If every key shows correctly here, it will work in the lock too.
//
// At power-up:  RED LED on 1 s  ->  GREEN LED on 1 s  (labelled on the OLED)
// Then press keys:
//   digits 0-9        -> GREEN LED flashes
//   A B C D * #       -> RED LED flashes
//   the key appears large on the OLED; Serial Monitor (115200) also logs it
//   hold '#' for 2 s  -> repeat the red / green LED check
//
// Wiring (connector plugged in reversed):
//   keypad rows 1-4    GPIO 16, 32, 33, 25
//   keypad columns 1-4 GPIO 26, 27, 14, 13
//   red LED GPIO 23, green LED GPIO 19 (220 ohm each), OLED SDA 21 / SCL 22
#include <Wire.h>
#include <U8g2lib.h>
#include <Keypad.h>

#define OLED_SH1106   1
#define OLED_FLIP     1
#define OLED_ROTATION (OLED_FLIP ? U8G2_R2 : U8G2_R0)
#if OLED_SH1106
  U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(OLED_ROTATION, U8X8_PIN_NONE);
#else
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(OLED_ROTATION, U8X8_PIN_NONE);
#endif

#define LED_RED    23
#define LED_GREEN  19

const byte ROWS = 4;
const byte COLS = 4;
byte rowPins[ROWS] = {16, 32, 33, 25};   // keypad rows 1-4 (same as the lock)
byte colPins[COLS] = {26, 27, 14, 13};   // keypad columns 1-4
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

char     history[17] = "";
uint8_t  pressCount  = 0;

void screen(const char *big, const char *line1, const char *line2) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 10, "KEYPAD + LED TEST");
  u8g2.drawHLine(0, 13, 128);
  u8g2.setFont(u8g2_font_logisoso24_tf);
  u8g2.drawStr(0, 42, big);
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 54, line1);
  u8g2.drawStr(0, 64, line2);
  u8g2.sendBuffer();
}

void ledCheck() {
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED, HIGH);
  screen("RED", "Red LED should be ON", "(GPIO 23)");
  Serial.println("LED check: RED on (GPIO23)");
  delay(1000);
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_GREEN, HIGH);
  screen("GREEN", "Green LED should be ON", "(GPIO 19)");
  Serial.println("LED check: GREEN on (GPIO19)");
  delay(1000);
  digitalWrite(LED_GREEN, LOW);
  screen("Ready", "Press any key", "hold # 2s: LED check");
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  Wire.begin(21, 22);
  u8g2.begin();
  keypad.setHoldTime(2000);
  delay(300);
  Serial.println("\nKeypad + LED test");
  ledCheck();
}

void loop() {
  char k = keypad.getKey();
  if (k) {
    bool digit = (k >= '0' && k <= '9');
    uint8_t led = digit ? LED_GREEN : LED_RED;

    uint8_t n = strlen(history);
    if (n >= 16) { memmove(history, history + 1, 15); n = 15; }
    history[n] = k; history[n + 1] = 0;
    pressCount++;

    char big[8], l1[24];
    snprintf(big, sizeof(big), "KEY %c", k);
    snprintf(l1, sizeof(l1), "%s LED  #%u", digit ? "GREEN" : "RED", pressCount);
    screen(big, l1, history);
    Serial.printf("key %c  (%s LED)\n", k, digit ? "green" : "red");

    digitalWrite(led, HIGH);
    delay(250);
    digitalWrite(led, LOW);
  }

  if (keypad.getState() == HOLD && keypad.key[0].kchar == '#') {
    while (keypad.getState() == HOLD) keypad.getKey();   // wait for release
    ledCheck();
  }
}
