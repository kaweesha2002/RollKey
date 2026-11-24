# Project Status & Development Roadmap: AVR Rolling Code Lock

## 1. Current System Architecture
This project is a low-level, high-security digital lock implemented entirely in **AVR Assembly** for the **ATmega328P**. It bypasses standard Arduino libraries to achieve microsecond-precise timing, minimal memory footprint (< 1.1KB Flash), and direct hardware register manipulation.

### Hardware Stack
* **Microcontroller:** ATmega328P (16MHz).
* **Interface:** 4-Bit Parallel LCD, Matrix Keypad.
* **Actuators & Indicators:** Solenoid lock (driven via external transistor/relay), Piezo buzzer for audio feedback, Red/Green status LEDs.
* **Power Delivery:** Dual-rail architecture. 5V logic supply for the MCU/LCD and an isolated 9V/12V supply for the solenoid to prevent inductive kickback and electromagnetic interference (EMI).

---

## 2. Core Implemented Features
* **Pseudo-Random Rolling Code:** Utilizes a 16-bit Linear Feedback Shift Register (LFSR) with the standard CCITT-16 polynomial (x^16 + x^14 + x^13 + x^11 + 1) to generate a unique 4-digit access code after every successful entry.
* **Hardware Entropy Injection:** Samples the hardware timer (`TCNT0`) during asynchronous human key presses to inject true randomness into the seed.
* **Non-Volatile Memory (EEPROM):** System state and the current LFSR seed are continuously saved to EEPROM (addresses `0x10` and `0x11`), ensuring the lock cannot be bypassed by power cycling.
* **Challenge-Response Deadlock:** After two consecutive failed attempts, the system enters a secure lockout state. Recovery requires calculating and inputting the **9's Complement** of the internal seed displayed on the LCD.
* **Hardware-Triggered Factory Wipe:** Pressing `*` from the standby state clears the EEPROM memory, flashes the Red LED, sounds a confirmation tone, and halts the system until a hard reset is performed.

---

## 3. Current Hardware Constraints & Mitigations

* **Electromagnetic Interference (EMI):** The initial prototype experienced LCD synchronization loss ("garbage text") when the solenoid actuated.
* **Resolution:** Mitigated at the hardware level by physically separating the logic power rail from the solenoid power rail and applying a **1N4007 Flyback Diode** directly across the solenoid terminals to suppress back-EMF spikes.

---

## 4. Proposed Future Developments

### Phase 1: Software & Logic Enhancements

| Feature | Description | Priority |
| :--- | :--- | :--- |
| **Input Timeout** | Implement a timer interrupt that clears the input buffer and returns to standby if the user takes longer than 10 seconds between key presses. | High |
| **Master Admin Code** | Hardcode a secure 6-digit master override in Flash memory that can bypass the deadlock state without requiring 9's complement math. | Medium |
| **Dynamic Polynomials** | Allow the system to cycle between different LFSR tap configurations based on the EEPROM wear-leveling count. | Low |

### Phase 2: Hardware & PCB Iterations

| Feature | Description | Priority |
| :--- | :--- | :--- |
| **I2C LCD Integration** | Transition from a 4-bit parallel LCD interface to an I2C backpack. This will free up 4 GPIO pins on the ATmega328P for additional sensors. | High |
| **Optoisolated Solenoid** | Upgrade the solenoid driver circuit to use an optocoupler (e.g., PC817). This provides total galvanic isolation between the MCU and the high-current lock. | Medium |
| **Battery Backup (UPS)** | Integrate a TP4056 module and a 18650 Li-ion battery to keep the logic side of the lock operational during main power failures. | Low |
