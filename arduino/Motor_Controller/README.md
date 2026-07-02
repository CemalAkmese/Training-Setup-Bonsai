# Motor Controller firmware

Firmware for the **ATbio UFR P02752** 3-axis stepper motor board. Each board drives one manipulator (left or right lick spout) with X, Y, and Z axes controlled through I²C port expanders (MCP23017).

Two identical boards run this firmware — one per spout. They receive position commands from the VR Task Arduino via parallel DIO lines and move the spout to the requested position.

## How it works

### Hardware architecture

```
Arduino Uno/Nano/Mega
        │
        ├── I²C bus (400 kHz) ──┬── MCP23017 @ 0x20 (Manipulator 1)
        │                       │     GPA0-GPB6 → motor enable/dir/step + limit switches
        │                       │
        │                       └── MCP23017 @ 0x21 (Manipulator 2, if dual mode)
        │                             same pin mapping on second expander
        │
        ├── DIO1–DIO6 (pins 3,5,7,9,11,15) ← 6-bit slot address from VR Task Arduino
        ├── DIO7 (pin 17) ← trigger input (active LOW pulse)
        ├── DIO8 (pin 19) → ready output (HIGH = idle, LOW = moving)
        │
        └── Serial (115200) ← USB terminal for manual control & position saving
```

### Port expander pin mapping

Each MCP23017 controls three stepper motors (X, Y, Z) through these pins:

| MCP pin | GPA/GPB | Motor 1 (X) | Motor 2 (Y) | Motor 3 (Z) |
|---------|---------|-------------|-------------|-------------|
| 0       | GPA0    | Switch 1    |             |             |
| 1       | GPA1    | Switch 2    |             |             |
| 2       | GPA2    |             | Switch 1    |             |
| 3       | GPA3    |             | Switch 2    |             |
| 4       | GPA4    |             |             | Switch 1    |
| 5       | GPA5    |             |             | Switch 2    |
| 6       | GPA6    | Enable      |             |             |
| 7       | GPA7    | Direction   |             |             |
| 8       | GPB0    | Step        |             |             |
| 9       | GPB1    |             | Enable      |             |
| 10      | GPB2    |             | Direction   |             |
| 11      | GPB3    |             | Step        |             |
| 12      | GPB4    |             |             | Enable      |
| 13      | GPB5    |             |             | Direction   |
| 14      | GPB6    |             |             | Step        |

### Motor parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| Steps per revolution | 200 | Standard 1.8° stepper |
| Lead screw pitch | 1.5 mm/rev | 1500 in firmware units (1/1000 mm) |
| Axis length | 6666 steps | Full travel range |
| I²C clock | 400 kHz | Fast mode |

### Position storage

The controller maintains 12 position slots, each storing all 6 axis positions (3 per manipulator):

| Slots | Storage | Purpose |
|-------|---------|---------|
| 1–8   | EEPROM (persistent) | Mouse-specific lick positions, saved via USB terminal |
| 9–12  | RAM (volatile) | Session-derived positions set by `SETMOUSE` command |

Slot 9–12 semantics (set by `SETMOUSE` relative to a mouse's lick position):

- **Slot 9**: lick Y − reach_1 mm (short reach)
- **Slot 10**: lick Y − reach_2 mm (medium reach)
- **Slot 11**: lick Y − reach_3 mm (full reach)
- **Slot 12**: lick Y − out mm (fully retracted / ITI position)

EEPROM layout: each slot occupies 24 bytes (6 axes × 4 bytes per long), starting at address 0.

### Speed settings

Four speed levels control the step pulse delay:

| Speed | Mega (µs) | Nano R4 (µs) |
|-------|-----------|---------------|
| 0     | 700       | 1000          |
| 1     | 600       | 700           |
| 2     | 400       | 500           |
| 3     | 180       | 250           |

## Main loop

```
loop()
  ├── DIO8 = HIGH (signal "ready")
  ├── handleLineCommands()    ← process multi-char serial commands (SETMOUSE, etc.)
  ├── USBcommand()            ← process single-char USB terminal commands
  ├── chkMotorOff()           ← disable motors after idle timeout
  └── if DIO7 == LOW and not in loadMode:
          DIOBefehl()         ← read DIO1-6 bit pattern, move to requested slot
```

The `loadMode` flag prevents DIO triggers from interrupting while positions are being uploaded from the PC over serial.

## Companion file

This firmware expects a companion file **`USBTerminal.ino`** in the same sketch folder (Arduino IDE compiles all `.ino` files in a folder together). That file provides:

- `USBcommand()` — single-character terminal interface for manual jogging
- `handleLineCommands()` — multi-character command parser (`SETMOUSE`, position save/load)
- `loadMode` flag — suppresses DIO trigger handling during serial uploads
- `printUSBinfo()`, `printPositionen()` — debug output helpers

> **Note:** `USBTerminal.ino` is not yet in this repository. It ships with the ATbio board and should be placed alongside `Motor_Controller.ino` in the same folder.

## Dependencies

- Adafruit MCP23X17 library v2.3.2
- Arduino EEPROM library (built-in)
- Arduino Wire library (built-in)

## Single vs. dual manipulator mode

Set `singleManipulator = HIGH` (default) for one manipulator, or `LOW` to enable the second MCP23017 at address 0x21. In dual mode, motors 4–6 mirror motors 1–3 on the second expander.
