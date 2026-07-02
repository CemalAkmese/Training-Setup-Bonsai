# VR Task INCODE firmware

Main trial controller for the INCODE head-fixed 2AFC licking task. Runs on an **Arduino Mega** and orchestrates the entire trial flow: parsing commands from Bonsai, cueing the mouse, detecting licks, delivering rewards, moving spouts, and reporting outcomes.

## Trial state machine

The core logic is a state machine driven by the `active` variable. Each trial progresses through these states:

```
active=0  IDLE
  │  Bonsai sends "1,X,Y\n" → parse buf[4] to set dirtouch, tone, freq
  │  taskstate → 1 → spouts extend to MOUSE_SLOT
  ▼
active=1  PRE-TONE WAIT
  │  Wait pretonewin ms (default 1000 ms)
  │  Then fire GO_CUE_TRIGGER → external Tone Arduino plays tone for cuedur ms
  │  EVT_PIN goes HIGH to mark cue onset on DAQ
  ▼
active=2  POST-TONE / CUE-OFF
  │  EVT_PIN goes LOW after tone ends
  │  Wait additional posttonewin ms (default 500 ms)
  ▼
active=4  RESPONSE WINDOW
  │  Duration: respwin ms (default 2000 ms)
  │  Continuously read lsense / rsense
  │
  ├──► Correct lick detected → active=5
  ├──► Wrong side lick:
  │      correction ON  → WhiteNoise(), set touchedother, stay in active=4
  │      correction OFF → active=6 (FalseAlarm)
  └──► Timeout → NoResponse() → active=10
  
active=5  GIVE REWARD
  │  Open correct solenoid (SOL_L or SOL_R)
  │  taskoutcome = 1 (hit) or 4 (corrected hit)
  ▼
active=7  REWARD DELIVERY
  │  Wait spoutopen ms (default 100 ms = solenoid open time)
  │  Close solenoid, retract spouts
  ▼
active=10  END TRIAL
  │  Wait endtrialdur ms (default 2000 ms)
  │  Print "dirtouch,taskoutcome\n" to serial
  │  Reset all flags → active=0
  ▼
  (next trial)
```

### dirtouch values

| Value | Meaning | Correct response |
|-------|---------|-----------------|
| 1     | Reward left | Left lick |
| 2     | Reward right | Right lick |
| 3     | Shaping — reward either | Any lick (auto-reward left) |
| 4     | Shaping — reward either | Any lick (auto-reward right) |

## Timing parameters

| Variable | Default | Description |
|----------|---------|-------------|
| `pretonewin` | 1000 ms | Delay before tone onset |
| `cuedur` | 500 ms (200 ms after reset) | Go-cue tone duration |
| `posttonewin` | 500 ms | Delay after tone before response window opens |
| `respwin` | 2000 ms | Response window duration |
| `spoutopen` | 100 ms | Solenoid open time (controls reward volume) |
| `noisedur` | 500 ms | White-noise punishment duration |
| `endtrialdur` | 2000 ms | Post-trial delay before reporting outcome |

## Lick sensor modes

Two sensor types are supported, selectable via `lickSensorMode`:

| Mode | Pins | Sensor type |
|------|------|-------------|
| `USE_CAP` (default) | 45 (left), 30 (right) | Capacitive touch sensors |
| `USE_STRAIN` | 43 (left), 44 (right) | Strain gauge / piezo |

Both are read with `digitalReadFast()` every loop iteration. The sensor signal is expected to be HIGH when the mouse is licking.

## Spout motor control

The Mega commands two ATbio motor controllers through parallel DIO lines. Each controller has 8 pins:

```
DIO1–DIO6  →  6-bit slot address (which position to go to)
DIO7       →  Trigger (idle HIGH, pulse LOW for 10 ms to start move)
DIO8       ←  Ready (HIGH = controller idle, LOW = moving)
```

Key functions:

| Function | What it does |
|----------|-------------|
| `clearBitsA/B()` | Set DIO1–6 LOW |
| `goToSlotA/B(slot)` | Set bit pattern for slot, fire trigger |
| `waitReadyA/B()` | Block until DIO8 goes HIGH (20 s timeout) |
| `extendSpouts()` | Move both controllers to MOUSE_SLOT |
| `retractSpouts()` | Move both controllers to SLOT_ITI (12) |
| `initAxesA/B()` | Home all axes (set DIO6 HIGH only) |

The `spoutsExtended` flag tracks current position to avoid duplicate commands.

## Audio system

Audio is offloaded to a separate **Tone_Generation Arduino** (not in this repo). The Mega communicates with it using two trigger lines:

| Pin | Signal | Function |
|-----|--------|----------|
| 5   | `GO_CUE_TRIGGER` | HIGH pulse → play go-cue tone (duration controlled by Mega) |
| 3   | `WHITE_NOISE_TRIGGER` | HIGH pulse → play white noise (duration = noisedur ms) |

The firmware also contains an LFSR-based noise generator (`generateNoise()`) from the original design, but the current architecture uses the external Arduino for all audio to avoid blocking the main loop with `delay()`.

## DAQ synchronization

| Pin | Signal | Purpose |
|-----|--------|---------|
| 46  | `EVT_PIN` | Pulsed HIGH at cue onset, LOW after cue ends — marks trial events |
| 48  | `BARCODE_PIN` | Sync barcode pulses → both imaging and behavioral DAQs |

## Session configuration

Three values must be set before each session (top of the `.ino` file):

```cpp
int MOUSE_SLOT = 1;           // EEPROM slot 1–8 for this mouse's lick position
int allowcorrection = 1;      // 1 = correction trials ON, 0 = OFF
LickSensorMode lickSensorMode = USE_CAP;  // or USE_STRAIN
```

## Known limitations

- `WhiteNoise()` uses `delay(noisedur)` which blocks the main loop for 500 ms. During this window, licks are not detected. A non-blocking version (`WhiteNoiseCheckCorrect`) has been developed but is not yet integrated into this version.
- The `delay(1000)` in `EndTask()` after serial print adds a fixed 1-second pause at every trial end.
- `buf[1]` pre-cue handling also uses blocking `delay(precuedur)`.

## Dependencies

- `digitalWriteFast` library — fast GPIO for time-critical lick reads and event pulses
