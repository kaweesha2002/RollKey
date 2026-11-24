# Hardware

Schematics, PCB files, BOM, and enclosure designs for the RollKey lock.

## Folder Structure
```
hardware/
├── schematics/
│   ├── rolling_lock_final_schematic_25V.png/.svg  ← CURRENT schematic
│   └── archive/    ← earlier revisions (see below)
├── wiring/
│   └── rolling_lock_wiring_connections.docx  ← point-to-point wiring table (wire gauges, currents)
├── photos/
│   └── type_c_module_annotated.jpg           ← annotated UPS / Type-C module pads
├── pcb/            ← PCB layout files (empty)
├── enclosure/      ← Fusion 360 files for laser-cut enclosure (empty)
├── bom/            ← Bill of Materials spreadsheet (empty)
└── README.md
```

### Schematic revisions (oldest → newest, judged from their contents)

| File | Change |
| :--- | :--- |
| `archive/rolling_lock_schematic_final` | First "final": separate UPS + battery blocks, boost |
| `archive/rolling_lock_schematic_with_boost_fuse` | Adds 3 A fuse before the boost |
| `archive/rolling_lock_schematic_one_block_ups` | UPS + on-board 2×18650 holder drawn as one block, 1N5822 flyback |
| `archive/rolling_lock_schematic_two_capacitors` | Two 470 µF caps (boost input + ESP32 VIN) |
| `archive/rolling_lock_final_schematic` | Flyback "1N4007 or 1N5822", optional 100 nF bypass caps |
| **`rolling_lock_final_schematic_25V`** | **Current.** Caps specified as 470 µF **25 V electrolytic** (matches the wiring doc) |

---

## Power Architecture (ESP32 Build)

```
Mains adapter (5 V 3 A, Type-C)
        │
        ▼
  [5 V UPS Module]  ←── 18650 Li-ion cell (backup)
        │
        ├─── 5 V ──► ESP32 + OLED + Keypad (logic rail)
        │
        └─── 5 V ──► [XL6009E1 Boost] ──► 9 V ──► Solenoid rail
```

Automatic switchover to battery on power cut — no code required.

---

## Solenoid Driver Circuit

- **MOSFET:** IRLZ44N (logic-level gate, V_GS(th) ~2 V — driven directly from ESP32 GPIO)
- **Flyback diode:** 1N4007 across solenoid terminals (suppress back-EMF)
- **Drive strategy:** Full duty at pull-in, then PWM hold (~30–40% duty) to reduce heat

---

## Enclosure Plan

- **Style:** Box with curved edges, shiny black finish
- **Manufacturing:** Laser cut
- **Tool:** Fusion 360
- **Mounting:** Lock mounts on the stationary part of the safe (not the door)
- **CAD models:** Download exact component models from GrabCAD / TraceParts

Place Fusion `.f3d` / `.dxf` files in `enclosure/`.

---

## Bill of Materials (Core Components)

| # | Component | Value / Part | Notes |
|---|---|---|---|
| 1 | ESP32 dev board | — | Main MCU |
| 2 | OLED display | 1.3″ I2C (SH1106) | Replaces 16×2 LCD |
| 3 | Matrix keypad | 4×4 | — |
| 4 | Solenoid lock | ~12.6 Ω coil | Pull-in at 9 V, ~0.7 A |
| 5 | MOSFET | IRLZ44N | Solenoid driver |
| 6 | Flyback diode | 1N4007 | Across solenoid |
| 7 | UPS module | 5 V in / 5 V out, Type-C | Holds 18650 cell |
| 8 | Boost converter | XL6009E1 | Set to 9 V output |
| 9 | 18650 cell ×2 (parallel) | 2000–3000 mAh | Power backup, in UPS module holder |
| 10 | Fuse | 3 A | Feed to boost |
| 11 | Capacitors | 2 × 470 µF 25 V electrolytic | Boost input + ESP32 VIN |
| 12 | LEDs, buzzer, resistors | Red/green LED, 220 Ω ×3, 10 kΩ gate pull-down | — |

Place full BOM spreadsheet in `bom/`.
