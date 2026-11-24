# Firmware — AVR (ATmega328P)

Pure AVR Assembly implementation of the rolling code lock.

## Target
- **MCU:** ATmega328P @ 16 MHz
- **Toolchain:** AVR-GCC / AVRA assembler + avrdude
- **Flash usage:** < 1.1 KB

## Pin Map

| Peripheral | Port/Pin | Function |
| :--- | :--- | :--- |
| MCU | ATmega328P | Main Controller |
| LCD Data | PD4 – PD7 | 4-Bit Data Bus |
| LCD RS | PD3 | Register Select |
| LCD EN | PD2 (or PB5) | Enable |
| Keypad Rows | PB0 – PB3 | Row Scan (Outputs) |
| Keypad Cols | PC0 – PC2 | Column Detect (Inputs) |
| Solenoid | PC5 | Lock Actuator |
| Buzzer | PD2 | Audio Feedback |
| Error LED (Red) | PC4 | Alarm |
| Status LED (Green) | PC3 | Wipe / OK |

## EEPROM Layout

| Address | Contents |
| :--- | :--- |
| `0x10` | LFSR seed (high byte) |
| `0x11` | LFSR seed (low byte) |

## LFSR Polynomial
CCITT-16: x^16 + x^14 + x^13 + x^11 + 1  
Tap mask: `0xB400`

## Building & Flashing
```bash
# Assemble
avra rollinglock.asm

# Flash via USBasp / Arduino-as-ISP
avrdude -c usbasp -p m328p -U flash:w:rollinglock.hex:i
```

## Source Files
Place your `.asm` source files in this directory.
