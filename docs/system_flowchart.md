# System Logic Flowchart

## AVR Rolling Code Lock — State Machine

```mermaid
graph TD
    classDef startEnd fill:#2E5A88,stroke:#90CAF9,stroke-width:2px,color:#fff;
    classDef process fill:#4A3B52,stroke:#CE93D8,stroke-width:1px,color:#fff;
    classDef decision fill:#544B29,stroke:#FFF59D,stroke-width:2px,color:#fff;
    classDef danger fill:#663333,stroke:#EF9A9A,stroke-width:2px,color:#fff;

    Start((Power On)) --> Init[Initialize Ports, LCD & EEPROM]
    Init --> Standby[STATE_STANDBY: Display Blank]

    Standby --> KeyScan{Key Pressed?}

    KeyScan -- # --> DisplayCode[LCD: 'CODE:' & Show Current Code]
    KeyScan -- * --> Wipe[SYSTEM_WIPE: LCD 'WIPE']

    Wipe --> EraseEEPROM[Overwriting EEPROM with 0xFF]
    EraseEEPROM --> Signal[Green LED ON & System Halt]

    DisplayCode --> Input[GET_INPUT_STRING: Enter 4 digits]
    Input --> Validate{Validate Rolling Code}

    Validate -- Match --> Unlock[UNLOCK_SYSTEM: Solenoid ON]
    Unlock --> UpdateSeed[LFSR Update & Save new Seed]
    UpdateSeed --> Delay[Display 'NEXT' Code & Wait]
    Delay --> Standby

    Validate -- No Match --> Error[HANDLE_FAILURE: Beep & Red LED]
    Error --> FailCount{Failures >= 2?}

    FailCount -- No --> Standby
    FailCount -- Yes --> Deadlock[STATE_DEADLOCK: LCD 'LOCK: XY']

    Deadlock --> RecoveryInput[User enters 9's Complement]
    RecoveryInput --> RecoveryCheck{Check Math}

    RecoveryCheck -- Correct --> ResetFail[Reset Fail Counter]
    ResetFail --> Standby

    RecoveryCheck -- Incorrect --> Lockout[Display '?' & Delay]
    Lockout --> Deadlock

    class Start,Standby startEnd;
    class Validate,FailCount,RecoveryCheck,KeyScan decision;
    class Deadlock,Error,Lockout,Wipe,EraseEEPROM danger;
```

---

## Operation Guide

### Standard Entry
1. Press `#` to begin.
2. Enter the 4-digit rolling code.
3. **Success:** Solenoid triggers, next code is generated.

### Deadlock Recovery
- After 2 failed attempts: LCD shows `LOCK: XY`
- Calculate the **9's Complement** of both digits (e.g., `27` → enter `72#`).

### System Wipe (Factory Reset)
1. Press `*` from Standby.
2. LCD shows `WIPE`, EEPROM is cleared, Green LED stays on.
3. Power-cycle the device; it restarts with the default seed.

> **ESP32 version:** After a wipe the device also clears all paired BLE devices and prompts for a new Bluetooth passkey to be typed on the keypad.
