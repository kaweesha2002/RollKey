// Solenoid test. Fires the lock on demand so you can check the MOSFET,
// boost converter and flyback diode on their own.
//
// Wiring (same as the main sketch):
//   GPIO4 -> 220 ohm -> IRLZ44N gate (10k gate -> GND)
//   Boost OUT+ (9 V) -> lock red wire; lock green wire -> MOSFET drain
//   MOSFET source -> GND. The ESP32 GND MUST be joined to the boost/UPS GND.
//
// Fire it:
//   - press the BOOT button on the ESP32, or
//   - Serial Monitor at 115200, send:
//       f = pull-in then PWM hold for 3 s (same as the final firmware)
//       p = plain on for 1 s (no PWM)
//       a = auto: fire every 5 s, send a again to stop
// The blue on-board LED (GPIO2) is lit while the gate is driven.

#define SOLENOID_PIN        4
#define BOOT_BTN            0
#define STATUS_LED          2
#define SOLENOID_PULL_MS    300
#define SOLENOID_HOLD_DUTY  100     // 0-255
#define SOLENOID_OPEN_MS    3000
#define SOLENOID_PWM_HZ     20000

bool autoFire = false;
unsigned long lastAuto = 0;

void gate(uint8_t duty) {
  ledcWrite(SOLENOID_PIN, duty);
  digitalWrite(STATUS_LED, duty ? HIGH : LOW);
}

void firePwm() {
  Serial.println("FIRE: full power 300 ms, then hold");
  gate(255);
  delay(SOLENOID_PULL_MS);
  gate(SOLENOID_HOLD_DUTY);
  delay(SOLENOID_OPEN_MS - SOLENOID_PULL_MS);
  gate(0);
  Serial.println("released");
}

void firePlain() {
  Serial.println("FIRE: plain on, 1 s");
  gate(255);
  delay(1000);
  gate(0);
  Serial.println("released");
}

void setup() {
  Serial.begin(115200);
  pinMode(BOOT_BTN, INPUT_PULLUP);
  pinMode(STATUS_LED, OUTPUT);
  ledcAttach(SOLENOID_PIN, SOLENOID_PWM_HZ, 8);
  gate(0);
  delay(500);
  Serial.println();
  Serial.println("Solenoid test ready. BOOT button or 'f' = PWM fire, 'p' = plain 1 s, 'a' = auto every 5 s");
}

void loop() {
  if (digitalRead(BOOT_BTN) == LOW) {
    delay(30);
    if (digitalRead(BOOT_BTN) == LOW) {
      firePwm();
      while (digitalRead(BOOT_BTN) == LOW) delay(10);
    }
  }
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'f') firePwm();
    else if (c == 'p') firePlain();
    else if (c == 'a') { autoFire = !autoFire; Serial.println(autoFire ? "auto ON" : "auto OFF"); lastAuto = 0; }
  }
  if (autoFire && millis() - lastAuto > 5000) {
    lastAuto = millis();
    firePwm();
  }
}
