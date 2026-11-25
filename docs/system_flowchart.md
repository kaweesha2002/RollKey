# System Flowcharts

How the RollKey lock behaves, taken from the main firmware
[`rolling_code_lock_esp32_alerts`](../firmware/esp32/rolling_code_lock_esp32_alerts/)
and the Android app. GitHub renders these diagrams automatically.

1. [Power-up and first setup](#1-power-up-and-first-setup)
2. [Main loop: code entry, unlock and failures](#2-main-loop-code-entry-unlock-and-failures)
3. [Deadlock challenge and hard lockout](#3-deadlock-challenge-and-hard-lockout)
4. [Wipe](#4-wipe)
5. [Push alert sender (background task)](#5-push-alert-sender-background-task)
6. [Phone pairing and code delivery](#6-phone-pairing-and-code-delivery)

---

## 1. Power-up and first setup

```mermaid
flowchart TD
    classDef io fill:#1f3b5c,stroke:#90CAF9,color:#fff;
    classDef step fill:#3b3350,stroke:#CE93D8,color:#fff;
    classDef dec fill:#4d4526,stroke:#FFF59D,color:#fff;

    A((Power on)) --> B[Start alert sender task<br/>queue 'wipe done' if flagged]
    B --> C[Init pins, solenoid off,<br/>OLED on I2C]
    C --> D{Owner numbers<br/>in NVS?}
    D -- No --> E[FIRST SETUP<br/>wipe PIN x2, restore number x2]
    E --> F[Save owner numbers<br/>code = restore number]
    D -- Yes --> G[Load seed from NVS]
    F --> H{Pair key<br/>in NVS?}
    G --> H
    H -- No --> I[SET PAIR KEY<br/>6 digits x2, not starting with 0]
    I --> J[Save key, forget old phones]
    H -- Yes --> K
    J --> K[Start Bluetooth NimBLE<br/>advertise as RollingLock]
    K --> L[Publish current code<br/>over Bluetooth]
    L --> M([Standby: OLED dark])

    class A,M io;
    class D,H dec;
    class B,C,E,F,G,I,J,K,L step;
```

- Values are typed twice; a mismatch asks again. Entry on the OLED is masked.
- The owner numbers are permanent. Only *Erase All Flash* on upload resets them.

---

## 2. Main loop: code entry, unlock and failures

```mermaid
flowchart TD
    classDef io fill:#1f3b5c,stroke:#90CAF9,color:#fff;
    classDef step fill:#3b3350,stroke:#CE93D8,color:#fff;
    classDef dec fill:#4d4526,stroke:#FFF59D,color:#fff;
    classDef bad fill:#5c2626,stroke:#EF9A9A,color:#fff;
    classDef good fill:#1f4d36,stroke:#A5D6A7,color:#fff;
    classDef alert fill:#5c4a1f,stroke:#FFCC80,color:#fff;

    S([Standby]) --> DL{In deadlock?}
    DL -- Yes --> CH[[Deadlock challenge<br/>see section 3]]
    DL -- No --> K{Key pressed?}
    K -- No --> S
    K -- "#" --> CE[CODE: enter 4 digits<br/>10 s per key]
    K -- "*" --> WP[[Wipe<br/>see section 4]]
    K -- other --> S

    CE --> TO{Timed out?}
    TO -- Yes --> TM[TIMEOUT, input cleared<br/>not counted as a failure] --> S
    TO -- No --> V{Code correct?}

    V -- Yes --> U1[Clear failures<br/>OPEN, green LED]
    U1 --> U2[Solenoid: full power 300 ms<br/>then ~40% hold, 3 s total<br/>alerts paused]
    U2 --> U3[Release lock]
    U3 --> A1[/Alert: Safe unlocked/]
    A1 --> U4[Step LFSR + keypress entropy<br/>save seed to NVS]
    U4 --> U5[Send new code to the<br/>paired phone]
    U5 --> S

    V -- No --> F1[ERROR, red LED, beep]
    F1 --> A2[/Alert: Wrong code entered/]
    A2 --> GP{First try after<br/>challenge recovery?}
    GP -- Yes --> S
    GP -- No --> FC[failures + 1]
    FC --> HL{Hard lockout<br/>active?}
    HL -- Yes --> S
    HL -- No --> L2{failures >= 2?}
    L2 -- No --> S
    L2 -- Yes --> A3[/Alert: Lock blocked<br/>after wrong codes/]
    A3 --> D1[Enter deadlock] --> S

    class S io;
    class DL,K,TO,V,GP,HL,L2 dec;
    class CE,TM,U4,U5,FC step;
    class U1,U2,U3 good;
    class F1,D1 bad;
    class A1,A2,A3 alert;
```

- The OLED wakes on `#` or `*` and goes dark again in standby.
- The next code is **never shown on the OLED**; it only goes to the paired phone.
- During hard lockout a correct code still opens the lock and clears the lockout.

---

## 3. Deadlock challenge and hard lockout

```mermaid
flowchart TD
    classDef step fill:#3b3350,stroke:#CE93D8,color:#fff;
    classDef dec fill:#4d4526,stroke:#FFF59D,color:#fff;
    classDef bad fill:#5c2626,stroke:#EF9A9A,color:#fff;
    classDef good fill:#1f4d36,stroke:#A5D6A7,color:#fff;
    classDef alert fill:#5c4a1f,stroke:#FFCC80,color:#fff;

    D0[Deadlock] --> C1[Pick 2 random digits<br/>hardware RNG]
    C1 --> C2[OLED: LOCK: XY  attempt n/2]
    C2 --> C3["User types 9's complement<br/>of each digit, then #"]
    C3 --> T{Timed out?}
    T -- Yes --> C4[TIMEOUT<br/>new challenge, attempt not used] --> C1
    T -- No --> OK{Answer correct?}
    OK -- Yes --> R1[Clear failures<br/>lock stays CLOSED]
    R1 --> R2["OLED: PRESS # + CODE<br/>next wrong code gets a grace pass"]
    R2 --> SB([Back to standby])
    OK -- No --> W1[attempts + 1]
    W1 --> W2{attempts >= 2?}
    W2 -- No --> W3[OLED: ?  red LED, beep<br/>new challenge] --> C1
    W2 -- Yes --> H1[LOCKED OUT<br/>CODE OR WIPE]
    H1 --> A4[/Alert: Lock fully locked out/]
    A4 --> SB

    class C1,C2,C3,C4,W1,W3 step;
    class T,OK,W2 dec;
    class R1,R2 good;
    class H1 bad;
    class A4 alert;
```

Example: the OLED shows `LOCK:27`, so the answer is `72#`.

---

## 4. Wipe

```mermaid
flowchart TD
    classDef step fill:#3b3350,stroke:#CE93D8,color:#fff;
    classDef dec fill:#4d4526,stroke:#FFF59D,color:#fff;
    classDef bad fill:#5c2626,stroke:#EF9A9A,color:#fff;
    classDef alert fill:#5c4a1f,stroke:#FFCC80,color:#fff;

    W0["'*' pressed"] --> A5[/Alert: Wipe attempted/]
    A5 --> W1[WIPE PIN: enter 4 characters]
    W1 --> T{Timed out?}
    T -- Yes --> SB([Standby, not counted])
    T -- No --> P{PIN correct?}
    P -- No --> F[[Counts as a wrong code<br/>see section 2]]
    P -- Yes --> W2[Code = restore number<br/>clear all failures]
    W2 --> W3[WIPE, green LED, 3 beeps<br/>solenoid stays off]
    W3 --> W4[CLEARING BT: forget all<br/>paired phones, erase pair key]
    W4 --> W5[Save 'wipe done' flag in NVS]
    W5 --> W6[Restart]
    W6 --> W7[Boot asks for a NEW pair key]
    W7 --> A6[/After Wi-Fi connects:<br/>Alert: Lock was wiped/]

    class W1,W2,W5,W6,W7 step;
    class T,P dec;
    class W3,W4 bad;
    class A5,A6 alert;
```

Phones that were paired before the wipe must **unpair** in Android Bluetooth settings and pair again with the new key. The app detects this case (see section 6).

---

## 5. Push alert sender (background task)

The lock code only calls `sendAlert(event)`, which returns immediately. A separate task does the network work, so the keypad, OLED and lock never wait.

```mermaid
flowchart TD
    classDef step fill:#3b3350,stroke:#CE93D8,color:#fff;
    classDef dec fill:#4d4526,stroke:#FFF59D,color:#fff;
    classDef bad fill:#5c2626,stroke:#EF9A9A,color:#fff;
    classDef good fill:#1f4d36,stroke:#A5D6A7,color:#fff;

    E0["sendAlert(event)<br/>from the lock code"] --> RL{Same event sent<br/>in the last 30 s?}
    RL -- Yes --> X1[Dropped]
    RL -- No --> Q[Put in queue, return at once]

    Q -.-> T0[Alert task waits for queue]
    T0 --> B{Solenoid<br/>energised?}
    B -- Yes --> B1[Wait] --> B
    B -- No --> W{Wi-Fi up?}
    W -- No --> W1[Turn Wi-Fi on, connect<br/>max 15 s]
    W1 --> W2{Connected?}
    W2 -- No --> RT
    W2 -- Yes --> N
    W -- Yes --> N{Time known?}
    N -- No --> N1[Get time by NTP]
    N1 --> TK
    N -- Yes --> TK{OAuth token<br/>still valid?}
    TK -- Yes --> F
    TK -- No --> J[Sign JWT with the service<br/>account key, RS256]
    J --> O[HTTPS: exchange JWT for<br/>an access token, cache ~1 h]
    O --> F[HTTPS POST to FCM HTTP v1<br/>topic message, channel lock_alerts]
    F --> OK{HTTP 200?}
    OK -- Yes --> S1[Sent]
    OK -- No --> RT{Second attempt<br/>already used?}
    RT -- No --> W
    RT -- Yes --> X2[Give up: alert lost<br/>lock unaffected]
    S1 --> M{More alerts<br/>queued?}
    X2 --> M
    M -- Yes --> T0
    M -- No --> OFF[Wi-Fi off] --> T0

    class Q,T0,B1,W1,N1,J,O,F step;
    class RL,B,W,W2,N,TK,OK,RT,M dec;
    class X1,X2 bad;
    class S1,OFF good;
```

- Only the **event type** is sent. Never a code, PIN, key or token.
- If the "Lock was wiped" alert fails, its NVS flag stays and it is retried on the next boot.
- The phone does not need to be on the same network; Google delivers the alert anywhere.

---

## 6. Phone pairing and code delivery

```mermaid
sequenceDiagram
    autonumber
    participant U as Owner
    participant P as RollKey app
    participant L as Lock (ESP32)
    participant G as Google FCM

    U->>P: Open app, enter app PIN / fingerprint
    P->>G: Subscribe to the lock's alert topic
    U->>P: Scan & connect
    P->>L: Find "RollingLock", connect over Bluetooth
    L-->>P: Pairing required
    P->>U: Android asks for the 6-digit pair key
    U->>P: Enter the pair key that was set on the lock's keypad
    P->>L: Pair (bonding + MITM + Secure Connections)
    P->>L: Subscribe to code updates, read code
    L-->>P: Current code (encrypted link only)
    Note over U,L: Later: owner types # + code on the keypad
    L-->>P: New code pushed after every unlock
    L->>G: Alert "Safe unlocked"
    G-->>P: Notification (even if the app is closed)

    Note over P,L: After a wipe the lock has forgotten this phone
    P->>L: Connect with the old pairing
    L-->>P: Rejected, link dropped before any code
    P->>U: "Lock was reset – pair again" + Bluetooth settings button
```
