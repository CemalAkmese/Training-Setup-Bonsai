# Training-Setup-Bonsai

Complete control system for the **INCODE VR licking task** — a head-fixed, two-alternative forced choice (2AFC) behavioral setup for mice. Includes the Bonsai-rx workflow (VR rendering, trial sequencing, data logging, camera control) and all Arduino firmware (task state machine, motor controllers, audio triggers).

## System overview

The rig uses **three Arduino boards** that work together:

```
┌─────────────────────────────────────────────────────────────────────┐
│                         HOST PC (Bonsai-rx)                        │
│  Sends trial commands ("1,X,Y\n")  ·  Receives outcomes ("d,o\n") │
└──────────────────────────────┬──────────────────────────────────────┘
                               │ USB-Serial 115200 baud
                               ▼
               ┌───────────────────────────────┐
               │   Arduino Mega  (VR Task)     │
               │   VR_Task_INCODE_updated.ino  │
               │                               │
               │  • Parses trial commands       │
               │  • Runs the trial state machine│
               │  • Reads lick sensors          │
               │  • Fires solenoid rewards      │
               │  • Sends event pulses to DAQ   │
               │  • Commands spout motors       │
               │  • Triggers external audio     │
               └──┬──────────────────────┬──────┘
         DIO pins │ (22-29)    (31-38)   │ Pins 3 & 5
                  ▼                ▼     ▼
    ┌──────────────────┐  ┌──────────────────┐  ┌────────────────────┐
    │ Motor Controller │  │ Motor Controller │  │ Tone_Generation    │
    │ A (left spout)   │  │ B (right spout)  │  │ Arduino            │
    │ Motor_Controller │  │ Motor_Controller │  │ (external, not in  │
    │ .ino             │  │ .ino             │  │  this repo)        │
    │                  │  │                  │  │                    │
    │ ATbio UFR P02752 │  │ ATbio UFR P02752 │  │ • Go-cue tone      │
    │ 3-axis stepper   │  │ 3-axis stepper   │  │ • White-noise burst │
    │ I²C port expander│  │ I²C port expander│  │                    │
    └──────────────────┘  └──────────────────┘  └────────────────────┘
```

## How a trial works

A single trial progresses through a state machine driven by the `active` variable:

```
  Bonsai sends "1,X,Y\n"
         │
         ▼
  ┌─────────────┐
  │ active = 0  │  Idle — parse serial, set dirtouch (1=left, 2=right,
  │ taskstate→1 │  3/4=shaping), set tone frequency, spouts extend
  └──────┬──────┘
         ▼
  ┌─────────────┐
  │ active = 1  │  Pre-tone wait (pretonewin ms)
  │  CueFunc()  │  then play go-cue via external Arduino
  └──────┬──────┘
         ▼
  ┌─────────────┐
  │ active = 2  │  Post-tone window (posttonewin ms)
  │  CueFunc()  │  EVT_PIN pulse marks cue onset for DAQ
  └──────┬──────┘
         ▼
  ┌─────────────┐
  │ active = 4  │  Response window (respwin ms)
  │ LickDetect  │  Read lsense / rsense
  └──┬───┬───┬──┘
     │   │   │
     │   │   └──── No lick within window ──► NoResponse() → active = 10
     │   │               taskoutcome = 2 (miss) or 0 (touched wrong + timed out)
     │   │
     │   └── Wrong side lick
     │        ├─ allowcorrection=0 → FalseAlarm() → white noise, retract, end
     │        └─ allowcorrection=1 → WhiteNoise(), set touchedother=1,
     │                                trial continues (mouse can self-correct)
     │
     └──── Correct lick ──► GiveReward() → active = 5 → 7
                              Open solenoid for spoutopen ms
                              taskoutcome = 1 (hit) or 4 (corrected hit)
                                      │
                                      ▼
                               EndReward()
                               Close solenoid, retract spouts
                                      │
                                      ▼
                               ┌─────────────┐
                               │ active = 10  │  End-trial delay (endtrialdur ms)
                               │  EndTask()   │  Serial.print("dirtouch,outcome")
                               │              │  Reset all flags → back to active=0
                               └──────────────┘
```

## Task outcome codes

| Code | Meaning |
|------|---------|
| 1    | **Hit** — correct lick, reward delivered |
| 2    | **Miss** — no response within window |
| 0    | **False alarm** — wrong side only (touchedother + timeout) |
| 4    | **Corrected hit** — wrong side first, then correct (correction mode) |

## Serial protocol (Bonsai ↔ VR Task Arduino)

**Bonsai → Arduino** — 6-character string: `"Z,X,Y\n"`

The key character is `buf[4]` (5th char), which sets the trial type:

| `buf[4]` | dirtouch | Tone? | Description |
|-----------|----------|-------|-------------|
| `A`       | random 1/2 | yes (freq_a or freq_b) | Context A — auditory-cued, random side |
| `B`       | random 2/1 | yes (reversed mapping) | Context B — auditory-cued, reversed |
| `T`       | 1 (left)   | yes (freq_a) | Tone + fixed left |
| `O`       | 2 (right)  | yes (freq_b) | Tone + fixed right |
| `L`       | 1 (left)   | no  | Left-only shaping (no tone, no pre-cue) |
| `R`       | 2 (right)  | no  | Right-only shaping |
| `N`       | 1 (left)   | no  | Same as L |
| `G`       | 2 (right)  | no  | Right + reward-cue dur |
| `C`       | random 3/4 | yes | Shaping — reward either side, random tone |
| `D`       | random 4/3 | yes | Shaping — reversed mapping |
| `M`       | 3 (either) | no  | Shaping — reward either, no tone |
| `S`       | 4 (either) | no  | Shaping — reward either, no tone |

`buf[1] == '2'` triggers a **pre-cue tone** (long pulse on GO_CUE_TRIGGER for precuedur ms) instead of starting a trial.

**Arduino → Bonsai** — `"dirtouch,taskoutcome\n"` (e.g., `"1,1"` = left hit)

## Spout movement logic

Spouts are controlled by two ATbio motor controllers, each driving a 3-axis stepper manipulator. The VR Task Arduino commands them via a parallel DIO bit pattern (6 data bits + 1 trigger + 1 ready).

**When spouts move depends on correction mode:**

| Event | `allowcorrection = 1` (ON) | `allowcorrection = 0` (OFF) |
|-------|---------------------------|----------------------------|
| Trial start | Extend to MOUSE_SLOT | Extend to MOUSE_SLOT |
| Correct lick (hit) | Retract to SLOT_ITI | Retract to SLOT_ITI |
| Wrong side lick | Stay extended, play white noise | Retract immediately (false alarm) |
| No response (miss) | Stay extended | Retract |

## Hardware pin map (Arduino Mega)

| Pin | Function | Direction |
|-----|----------|-----------|
| 2   | Left solenoid (reward valve) | OUTPUT |
| 3   | White-noise trigger → Tone Arduino | OUTPUT |
| 5   | Go-cue trigger → Tone Arduino | OUTPUT |
| 22–29 | Motor Controller A (left): DIO1–DIO7 out, DIO8 ready in | MIXED |
| 30  | Capacitive sensor right | INPUT |
| 31–38 | Motor Controller B (right): DIO1–DIO7 out, DIO8 ready in | MIXED |
| 40  | Right solenoid (reward valve) | OUTPUT |
| 41  | Piezo TTL trigger | OUTPUT |
| 42  | Lever input | INPUT |
| 43  | Strain gauge left | INPUT |
| 44  | Strain gauge right | INPUT |
| 45  | Capacitive sensor left | INPUT |
| 46  | Event pulse → DAQ | OUTPUT |
| 48  | Barcode sync → both DAQs | OUTPUT |

## Motor controller DIO slot encoding

Each motor controller reads a 6-bit pattern on DIO1–DIO6 then responds to a LOW→HIGH trigger on DIO7. The controller signals completion by driving DIO8 HIGH.

| Slot | DIO6 | DIO5 | DIO4 | DIO3 | DIO2 | DIO1 | Use |
|------|------|------|------|------|------|------|-----|
| 1    | 0    | 0    | 1    | 1    | 1    | 0    | Mouse lick position (EEPROM) |
| 2    | 0    | 0    | 1    | 1    | 0    | 1    | Mouse lick position (EEPROM) |
| 3    | 0    | 0    | 1    | 1    | 0    | 0    | Mouse lick position (EEPROM) |
| 4    | 0    | 0    | 1    | 0    | 1    | 1    | Mouse lick position (EEPROM) |
| 5    | 0    | 0    | 1    | 0    | 1    | 0    | Mouse lick position (EEPROM) |
| 6    | 0    | 0    | 1    | 0    | 0    | 1    | Mouse lick position (EEPROM) |
| 7    | 0    | 0    | 1    | 0    | 0    | 0    | Mouse lick position (EEPROM) |
| 8    | 0    | 0    | 0    | 1    | 1    | 1    | Mouse lick position (EEPROM) |
| 12   | 0    | 1    | 0    | 1    | 0    | 0    | ITI retracted position (RAM) |
| home | 1    | 0    | 0    | 0    | 0    | 0    | Home all axes |

Slots 1–8 are persistent (EEPROM). Slots 9–12 are RAM-only reach/ITI positions computed at runtime by the `SETMOUSE` command.

---

## Bonsai workflow

The Bonsai-rx workflow (`VR_2Corridors.bonsai`) is the central orchestrator. It renders the VR environment, reads the running wheel, sequences trials, talks to all three Arduinos, records video from two cameras, logs behavioral data, and records the DAQ. Below is a map of every major group and how they connect.

### Top-level architecture

```
┌───────────────────────────────────────────────────────────────────────┐
│                       Bonsai Workflow (top level)                      │
│                                                                       │
│  ┌─────────────┐  ┌─────────────┐  ┌────────────────┐                │
│  │CreateSerial  │  │CreateSerial │  │ CreateArduino  │                │
│  │"Task" COM4  │  │"Nano" COM1  │  │ "Wheel" COM5   │                │
│  │115200 baud  │  │9600 baud    │  │ Firmata 57600  │                │
│  └─────────────┘  └─────────────┘  └────────────────┘                │
│        ↕                ↕                  ↕                          │
│   Arduino Mega     Arduino Nano       Arduino Firmata                 │
│   (VR Task)        (camera trigger)   (wheel encoder)                 │
│                                                                       │
│  ┌──────┐ ┌──────────────────┐ ┌──────────────────┐ ┌─────────┐     │
│  │ DAQ  │ │  Initialize      │ │   PylonCamera    │ │  Clock  │     │
│  │group │ │  Subjects        │ │   (2 cameras)    │ │         │     │
│  └──────┘ └──────────────────┘ └──────────────────┘ └─────────┘     │
│                                                                       │
│  ┌──────────────┐  ┌──────────────────────────────────────────┐      │
│  │ Plot         │  │                  VR                       │      │
│  │ Performance  │  │  (main trial loop — see below)           │      │
│  └──────────────┘  └──────────────────────────────────────────┘      │
│                                                                       │
│  Ready → 5s timer → starts wheel + cameras + trial loop              │
│  End key → stops cameras → 10s delay → stops recording               │
│  F5 or 1-hour timer → stops entire workflow                          │
└───────────────────────────────────────────────────────────────────────┘
```

### Serial ports and hardware connections

| Bonsai name | COM port | Baud | Protocol | Connected to |
|-------------|----------|------|----------|-------------|
| `Task`      | COM4     | 115200 | Line-based `\r\n` | Arduino Mega (VR Task firmware) |
| `Nano`      | COM1     | 9600   | Line-based `\r\n` | Arduino Nano (camera trigger generator) |
| `Wheel`     | COM5     | 57600  | Firmata | Arduino reading rotary encoder on analog pin 0 |

### DAQ group

NI-DAQmx analog input on `Dev1`, sampling at **1000 Hz**, 11 channels in NRSE mode. The data is published to a `BehaviorSubject` called `Daqdata` and optionally saved as a binary matrix file.

| Channel | DAQ pin | Signal |
|---------|---------|--------|
| StartFramePV | ai4 | Prairie View frame trigger |
| Solenoid(8) | ai8 | Left solenoid voltage |
| Solenoid(9) | ai9 | Right solenoid voltage |
| Cap(10) | ai10 | Capacitive sensor left |
| Cap(11) | ai11 | Capacitive sensor right |
| Tone(12) | ai12 | Tone/audio signal |
| Servo(13) | ai13 | Servo/motor signal (legacy name) |
| StrainG(6) | ai6 | Strain gauge left |
| StrainG(7) | ai7 | Strain gauge right |
| WheelEncoder(14) | ai14 | Wheel encoder voltage |
| VideoFrames(15) | ai15 | Video frame sync pulses |

### Initialize Subjects

All reactive `BehaviorSubject` variables are initialized here. These act as the shared state bus — any group can subscribe to or multicast into them.

| Subject | Initial value | Purpose |
|---------|--------------|---------|
| `Ready` | `false` | System armed flag; goes `true` after 5 s boot delay |
| `TaskState` | `false` | `true` while Arduino is running a trial |
| `Getwheeldata` | `false` | Enables/disables wheel encoder reading |
| `TimeOut` | `false` | Fires when mouse doesn't reach task zone in time |
| `ToneState` | `false` | `true` when mouse reaches tone-trigger position |
| `AuditoryCue` | `"low"` | Current auditory cue: `"low"` or `"high"` |
| `Context` | configurable (`"A"`) | Current context: `"A"` or `"B"` |
| `CohortType` | configurable (`"spatial"`) | `"spatial"` or `"auditory"` — determines rule mapping |
| `Corridor` | configurable (`"Corr_test"`) | Which Blender scene to render |
| `Direction` | (from StringA) | `"R"` or `"L"` — rewarded lick side this trial |
| `StringA` | configurable (`"R"`) | Serial character for Context A |
| `StringB` | configurable (`"L"`) | Serial character for Context B |
| `Tone` | `"H"` | Tone identifier sent to Arduino (`"H"` or `"T"`) |
| `RandomTrials` | configurable (`false`) | Enable randomized trial ordering |
| `CatchTrials` | configurable (`false`) | Enable catch trials |
| `needsCorrection` | `false` | Whether last trial needs a correction repeat |
| `TaskOut` | `0` | `1` = trial active, `0` = idle |
| `HitNum` | `0` | Running hit count |
| `MissNum` | `0` | Running miss count |
| `FANum` | `0` | Running false alarm count |
| `TimeOutNum` | `0` | Running timeout count |
| `TrialNumber` | `0` | Current trial index |
| `Within2Sec` | `0` | Count of outcome-code-4 events |
| `SwitchTask` | `false` | Corridor switch flag (disabled path) |

### PylonCamera group

Two Basler cameras (`a2A1920-160umBAS`) captured via Pylon, triggered externally by the Nano Arduino. The Nano receives a `"1"` over serial when `Ready` goes true, and another `"1"` when `StopCamTrigger` fires (End key). The framerate (default 30 fps) is sent to the Nano as its trigger rate.

| Camera | Serial number | Parameter file | Video output |
|--------|--------------|----------------|-------------|
| Camera 1 | 40238998 | `trigger_highgain.pfs` | `D:\Data\vid1_<timestamp>.mp4` |
| Camera 2 | 40238985 | `trigger_mediumgain.pfs` | `D:\Data\vid2_<timestamp>.mp4` |

Both use FFmpeg NVENC hardware encoding: `h264_nvenc -preset:v p7 -tune:v hq -rc:v vbr -cq:v 40`. Each camera also logs grab metadata (skipped frames, image number, timestamp, errors) to `grab1.csv` / `grab2.csv`. Video writers are gated by a `RecordVideo` boolean and stopped by the `StopRecording` subject.

### VR group — the main trial loop

This is the core behavioral loop. It repeats indefinitely, running one trial per iteration:

```
   ┌──────────────────────────────────────────────────────┐
   │                     VR Group                          │
   │                                                       │
   │  Timer(ITI) ──► ReadWheel ──► Concat ──► Last ──┐    │
   │                                                  │    │
   │       ┌──────────── Repeat ◄─────────────────────┘    │
   │       │                                               │
   │       ▼                                               │
   │  ElementIndex ──► TrialNumber                         │
   │                                                       │
   │  Parallel groups (always running):                    │
   │  ┌────────────┐ ┌────────────┐ ┌──────────────────┐  │
   │  │ DisplayVR  │ │ ToggleTask │ │     LogData      │  │
   │  │ (render    │ │ (trial     │ │ (CSV + DAQ       │  │
   │  │  corridor) │ │  control)  │ │  recording)      │  │
   │  └────────────┘ └────────────┘ └──────────────────┘  │
   │                                                       │
   │  TaskStartPos ──► BehaviorSubject "TaskStart" (0.07)  │
   │  ToneStartPos ──► BehaviorSubject "ToneStart" (-0.7)  │
   └──────────────────────────────────────────────────────┘
```

#### ReadWheel subgroup

Reads the rotary encoder via Arduino Firmata (`Wheel`, analog pin 0), converts voltage (0–1023 → 0–4.67 V), computes inter-sample differences, filters out jumps (>1 V or backward >1 V), rescales to distance (±4.67 V → ±0.628 arbitrary units), applies a moving-average smoothing window (default 10 samples), divides by a gain factor (default −1), and accumulates into a running position (`MousePos`).

The position is clamped: it cannot go below `StartPos` (default −0.77) via a `Max` scan. When `MousePos` exceeds `EndPos` (default 0.1), the trial's wheel-reading phase ends. There is also a configurable timeout timer (default 10 s) — if the mouse doesn't reach the end position in time, the `TimeOut` subject fires.

Key published subjects: `EncoderVoltage`, `PollDist` (per-sample displacement), `MousePos` (accumulated position), `StartPos`, `EndPos`, `Offset`.

#### DisplayVR subgroup

Renders the 3D corridor using BonVision's cubemap pipeline:

1. `RenderFrame` triggers each OpenGL frame.
2. A `CubemapCamera` is created at the origin.
3. The camera's Z translation is bound to `MousePos` — as the mouse runs, the view moves forward through the corridor.
4. The `Corridor` subject selects which Blender scene to render (`Corr_test` or `Corr_test_context2`).
5. The cubemap is rendered then projected onto **three viewports** (left / center / right, each 33.3% of the 3240×1920 window), simulating a surround display.

Viewport layout (BonVision ViewWindow → DrawViewport):

| Viewport | X offset | Width | Y rotation | Translation | Simulates |
|----------|----------|-------|------------|-------------|-----------|
| Left     | 0.0      | 0.333 | +75°       | (−20, 0, −5) | Left monitor |
| Center   | 0.334    | 0.333 | 0°         | (0, 0, −20)  | Front monitor |
| Right    | 0.667    | 0.333 | −75°       | (20, 0, −5)  | Right monitor |

A `gate` object in the Blender scene is moved vertically based on `MousePos` using `SceneTransform` + `UpdateTransform`, creating the illusion of approaching/passing a gate as the mouse runs.

#### ToggleTask subgroup

Contains five sub-groups that manage the trial-by-trial logic:

**1. ToggleCorridors** — Decides context, corridor, auditory cue, and rewarded direction each trial.

On each new trial (when `TrialNumber` increments and `needsCorrection` is false and `Ready` is true):
- Generates a random integer 0–4 (seeded by timestamp ticks).
- Maps to context: even → `"A"`, odd → `"B"` → publishes to `Context`.
- Maps context to corridor: `"A"` → `Corr_test`, `"B"` → `Corr_test_context2` → publishes to `Corridor`.
- Maps to auditory cue: <2 → `"low"`, ≥2 → `"high"` → publishes to `AuditoryCue`.
- Determines tone character: `"high"` → `"H"`, `"low"` → `"T"` → publishes to `Tone`.

The rewarded direction depends on `CohortType`:
- **Spatial cohort**: direction is determined by **context** (A → `"R"`, B → `"L"`). Auditory cue is present but uninformative.
- **Auditory cohort**: direction is determined by **tone frequency** (high → `"R"`, low → `"L"`). Visual context is present but uninformative.

This implements the INCODE two-cohort design where one cue dimension is rule-defining and the other is irrelevant.

**2. SendTask** — Sends serial commands to the Arduino Mega.

When `TaskState` goes `true`:
- Sets `TaskOut` to `1`.
- Combines `TaskOut` with `Direction` (e.g., `"1"` + `","` + `"R"`) and writes to the `Task` serial port.
- When `ToneState` goes `true`, sends a pre-cue tone command: `"2"` + `","` + `Tone` (e.g., `"2,H"`).

When `TaskState` goes `false` (trial ended): sets `TaskOut` to `0`.

**3. Detach Wheel** — Disables wheel reading while the Arduino is processing a trial.

Subscribes to `TaskState`. When it toggles, computes the difference between successive integer-cast values. If the result equals 1 (false→true transition), publishes `false` to `Getwheeldata`, pausing the wheel accumulator so the mouse's position is frozen during the lick-detection phase.

**4. ReceiveTask** — Reads the Arduino's response and resets state for the next trial.

Listens to serial on the `Task` port. Also subscribes to `TimeOut` — when the mouse doesn't reach the task zone, it injects `"3,3"` as a synthetic timeout response (outcome code 3).

Both real and synthetic responses are merged into `FromTask`. The workflow filters for 3-character strings (e.g., `"1,1"`, `"2,0"`, `"3,3"`), publishes them to `TaskFeedback`, then:
- Sets `TaskState` → `false` (trial over).
- Resets `StartPos` back to the `EndPos` value (0.1) so the mouse starts the next corridor run from the current position.

**5. TaskPosition** — Determines position-based triggers for the task and tone.

Continuously compares `MousePos` against two thresholds:
- `TaskStart` (default 0.07): when exceeded, `TaskState` → `true` (fires the trial on the Arduino).
- `ToneStart` (default −0.7): when exceeded, `ToneState` → `true` (fires the pre-cue tone).

Both use `DistinctUntilChanged` to fire only on transitions.

#### PlotPerformance subgroup

Subscribes to `TaskFeedback`, parses `"dirtouch,outcome"`, and routes by outcome code:

| Outcome | Counter subject | Also sets `needsCorrection` to |
|---------|----------------|-------------------------------|
| 1 (hit) | `HitNum` += 1 | `false` (no correction needed) |
| 2 (miss) | `MissNum` += 1 | `true` |
| 0 (FA) | `FANum` += 1 | `true` |
| 3 (timeout) | `TimeOutNum` += 1 | `true` |
| 4 (within-2s / corrected) | `Within2Sec` += 1 | `true` |

All four counters are combined into a single `FA-Miss-Hit-TimeOut` subject for live display.

The `needsCorrection` flag feeds back into `ToggleCorridors` — when `true`, the next trial repeats the same context/direction instead of randomizing.

#### LogData subgroup

Three parallel data streams, each gated by `SaveData` and `Ready` booleans:

| File | Path | Contents | Format |
|------|------|----------|--------|
| Meta | `D:\Data\Cemal\meta.csv` | Per-trial: StartPos, EndPos, TaskStart, dirtouch, outcome, corridor, direction string, trial number, timestamp | CSV with headers |
| Data | `D:\Data\Cemal\dat.csv` | Per-sample: PollDist, EncoderVoltage, timestamp, MousePos, corridor, direction string, trial number, TaskState | CSV with headers |
| DAQ | `D:\Data\Cemal\Daq` | Raw analog input matrix from all 11 DAQ channels | Binary (MatrixWriter, ColumnMajor) |

All three stop recording when `StopRecording` fires (End key + 10 s delay).

### BonVision resources

Two Blender corridor scenes and 24 textures are loaded at startup:

| Resource | File | Purpose |
|----------|------|---------|
| `Corr_test` | `Corridors\Corr_test.blend` | Context A corridor |
| `Corr_test_context2` | `Corridors\Corr_test_context2.blend` | Context B corridor |

Textures include context wall patterns (`Context1_v3`, `Context2_v3`, and their opposites), floor textures, rule zone walls, gratings (`vertGrat`, `horGrat`), fractal noise patterns (`f1`–`f4`, `fwn1`–`fwn4`), and utility textures (`white`, `black`, `grayEnd`, `plaid`). All are loaded as RGBA with linear filtering and vertical flip.

### Startup and shutdown sequence

**Startup:**
1. Serial ports and Firmata open.
2. DAQ starts continuous acquisition.
3. OpenGL window opens on the fourth display (3240×1920).
4. BonVision resources and textures load.
5. Initialize Subjects sets all defaults.
6. 5-second boot timer fires → `Ready` = `true` → wheel reading starts, camera triggers begin, trial loop begins.

**Shutdown (normal):**
1. Press **End** key → `StopCamTrigger` fires → Nano stops triggering cameras.
2. 10 s later → `StopRecording` fires → video writers and CSV writers close.
3. Press **F5** (in the GL window) or wait 1 hour → workflow stops entirely.

### Full data flow for one trial

```
1.  ITI timer expires
2.  ReadWheel begins accumulating MousePos from StartPos (−0.77)
3.  DisplayVR renders the corridor, camera moves with MousePos
4.  Mouse runs forward on the wheel...
     │
     ├─ MousePos > ToneStart (−0.7)
     │   → ToneState = true
     │   → SendTask writes "2,H\n" or "2,T\n" to Arduino (pre-cue tone)
     │
     ├─ MousePos > TaskStart (0.07)
     │   → TaskState = true
     │   → DetachWheel freezes wheel reading
     │   → SendTask writes "1,R\n" or "1,L\n" to Arduino (start trial)
     │   → Arduino runs trial state machine (see Arduino README)
     │
     ├─ EITHER: Arduino responds "dirtouch,outcome\n"
     │   → ReceiveTask parses it → TaskFeedback
     │   → PlotPerformance increments counters, sets needsCorrection
     │   → TaskState = false, StartPos reset
     │
     └─ OR: Timeout timer expires (mouse didn't reach task zone)
         → TimeOut fires → ReceiveTask injects "3,3"
         → Same downstream processing as above
     
5.  ReadWheel phase ends (MousePos > EndPos or timeout)
6.  Concat → Last emits → Repeat loops → TrialNumber increments
7.  ToggleCorridors picks new context/corridor/direction (unless correction)
8.  Back to step 1
```

---

## Repository structure

```
Training-Setup-Bonsai/
├── README.md                              ← you are here
├── VR_2Corridors.bonsai                   ← main Bonsai workflow (not yet in repo)
├── Corridors/
│   ├── Corr_test.blend                    ← Context A corridor (Blender 2.79b)
│   └── Corr_test_context2.blend           ← Context B corridor
├── Textures/                              ← wall/floor/grating textures (.jpg/.png)
├── pylon settings/                        ← Basler camera .pfs config files
├── arduino/
│   ├── Motor_Controller/
│   │   ├── README.md                      ← motor controller documentation
│   │   └── Motor_Controller.ino           ← ATbio stepper controller firmware
│   │       (companion: USBTerminal.ino — not yet in repo)
│   ├── VR_Task_INCODE/
│   │   ├── README.md                      ← VR task documentation
│   │   └── VR_Task_INCODE_updated.ino     ← main trial state machine
│   └── VR_Task_INCODE_updated.ino         ← duplicate of above (legacy copy)
```

## Per-session checklist

**Arduino setup:**
1. Power on both motor controllers; wait for `DIO8 = HIGH` (ready).
2. Open `VR_Task_INCODE_updated.ino`, set `MOUSE_SLOT` to the correct EEPROM slot for today's mouse.
3. Set `allowcorrection` (1 for early training, 0 for criterion sessions).
4. Set `lickSensorMode` (`USE_CAP` or `USE_STRAIN`).
5. Upload to Mega. On boot it homes both manipulators and retracts spouts.

**Bonsai setup:**
6. Open `VR_2Corridors.bonsai`.
7. Set `InitialContext` (`"A"` or `"B"`), `CohortType` (`"spatial"` or `"auditory"`).
8. Set `CorridorA` / `CorridorB` to the correct Blender scene names.
9. Set `StringA` / `StringB` to the matching serial direction characters.
10. Set `SaveData` = `true`, verify file paths (`datafilename`, `metafilename`, `daqfilename`).
11. Set `RecordVideo` = `true` if recording cameras.
12. Verify COM ports: `Task` = COM4, `Nano` = COM1, `Wheel` = COM5.
13. Start the workflow. After 5 s the system arms and trials begin.
14. Press **End** to stop cameras; press **F5** (in GL window) or wait 1 hour to end.

## Dependencies

**Arduino:**
- Arduino IDE 2.3.6+
- `digitalWriteFast` library (VR Task)
- `Adafruit MCP23X17` library v2.3.2 (Motor Controller)
- External Tone_Generation Arduino (firmware not in this repo)

**Bonsai-rx (v2.9.0):**
- Bonsai.System (serial ports, CSV writers)
- Bonsai.Arduino (Firmata — wheel encoder)
- Bonsai.DAQmx (NI analog input)
- Bonsai.Shaders + Bonsai.Shaders.Rendering (OpenGL, cubemap, scene rendering)
- BonVision (VR corridor display, ViewWindow, DrawViewport)
- Bonsai.Pylon (Basler camera capture)
- Bonsai.Vision (video writer — disabled legacy path)
- Bonsai.FFmpeg (NVENC video encoding)
- Bonsai.Dsp (rescale, matrix writer)
- Bonsai.Numerics (random number generation, distributions)
- Bonsai.Scripting.Expressions (inline C# expressions)
- Bonsai.Windows.Input (keyboard hotkeys — End key)

**Hardware:**
- NI DAQ (Dev1, 11 analog input channels)
- 2× Basler a2A1920-160umBAS cameras
- 3-monitor surround display (3240×1920 total, fourth display device)
- Running wheel with analog rotary encoder

## Related projects

- **Bonsai workflow**: BonVision VR corridor rendering + trial sequencing
- **INCODE-mouse_behavior**: Python analysis pipeline for behavioral data
- **FreiPose_Recorder**: Multi-camera video acquisition

## Authors

Cemal Akmese (Diester/Bödecker Lab, University of Freiburg)
Hardware design: Luca Habelt, ATbio Uni Freiburg
Motor controller firmware: ATbio (js), updated by Luca Habelt
