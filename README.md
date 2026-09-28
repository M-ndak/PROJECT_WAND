# PROJECT_WAND— Gesture & Color Interactive Wand

A real, working "magic wand" built with an Arduino Uno R3, an IMU, a touch sensor, and (originally) a color sensor + IR proximity sensor — streaming live gesture and sensor data over serial to a Python/Pygame app that renders spell-like particle animations in real time.

Built in **just 40 minutes** for the final round of **Circuitry Maze 3.0**, an inter-college hardware competition held at **Thadomal Shahani Engineering College**, themed around **Harry Potter / Hogwarts**. 🪄

🏆 **Secured 2nd place** in the final round.

---

## Overview

The wand detects physical gestures (swishes, flicks, shakes, twirls, thrusts) using an onboard IMU, and reports proximity of nearby objects using an IR sensor. A touch sensor puts the wand to sleep or wakes it up. All of this is streamed over serial (USB) to a PC, where a Python visualizer turns each gesture into a themed particle "spell" animation — swish for a sweeping arc, flick for a burst, shake for chaos, twirl for a spiral, thrust for a shockwave ring.

No RTOS was needed — the Arduino firmware uses a lightweight cooperative `millis()`-based scheduler instead, since the whole system comfortably fits within simple timed polling loops.

## Hardware

| Component | Role |
|---|---|
| Arduino Uno R3 | Main controller / firmware |
| IMU (MPU6050) | Gesture recognition (swish, flick, shake, twirl, thrust) |
| Touch sensor (TTP223) | Sleep / wake toggle |
| IR proximity sensor (digital) | Detects nearby objects (NEAR / FAR) |
| Color sensor (TCS34725) | *(used in an earlier version — later removed to simplify)* |

### Wiring

| Part | Uno pin |
|---|---|
| IMU SDA / SCL | A4 / A5 |
| Touch sensor OUT | D2 |
| IR proximity OUT | D3 |
| All sensor VCC / GND | 5V / GND |

## How It Works

1. **Firmware** (`wand.ino`) continuously polls the IMU (100 Hz) and proximity sensor, detects gesture patterns using angular velocity and acceleration thresholds, debounces the touch sensor for sleep/wake, and streams simple comma-separated events over serial (9600 baud):
   - `S,<0|1>` — awake/asleep state
   - `G,<NAME>` — gesture detected (`SWISH_L`, `SWISH_R`, `FLICK_UP`, `FLICK_DOWN`, `SHAKE`, `TWIRL`, `THRUST`)
   - `P,<NAME>` — proximity event (`NEAR`, `FAR`)
   - Human-readable `#`-prefixed debug lines for live monitoring in the Serial Monitor

2. **PC visualizer** (`sim.py`, Python + Pygame) reads the serial stream and renders a distinct particle animation for each gesture and proximity event, along with a live "awake/asleep" HUD and a pulsing warning border when an object is detected nearby.

## Gestures

| Gesture | Motion |
|---|---|
| SWISH_R / SWISH_L | Quick horizontal sweep |
| FLICK_UP / FLICK_DOWN | Quick vertical flick |
| SHAKE | Rapid back-and-forth motion |
| TWIRL | Spin along the wand's length |
| THRUST | Sharp forward jab |

## Running It

**Arduino side:** flash `wand.ino` to the Uno R3 via the Arduino IDE.

**PC side:**
```bash
pip install pygame pyserial
python sim.py
```
Use `--demo` to try it with keyboard input, no hardware required.

## Event & Team

- **Event:** Circuitry Maze 3.0 (Inter-college hardware/circuitry competition)
- **Theme:** Harry Potter / Hogwarts
- **Venue:** Thadomal Shahani Engineering College
- **Round:** Final round
- **Build time:** 40 minutes
- **Result:** 🥈 2nd Place

## Future Improvements

- Re-add the color sensor for color-reactive spell effects
- Add more gestures (circles, figure-eights) via a lightweight motion classifier
- Add sound effects per gesture
- Bluetooth/wireless version to remove the USB tether
