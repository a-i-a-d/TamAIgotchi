# Agent documentation — standing rules for this codebase

Rules for agents (and humans) working on the TamAIgotchi UI. These come from
the decisions made in the UI restructure (#29) and are the contract that keeps
the display correct. **Do not break them** — each one fixes a real bug that
re-appeared when a rule was relaxed.

## Single-render-pass rule (pitfall 5)

One `Display::render()` per frame = **status bar + alien + bubble**, with
exactly **one `clearDisplay()` + one `display()`**.

- `Display::render()` (display.cpp) is the ONLY place that calls
  `panel_.clearDisplay()` / `panel_.display()`. Every state change (status
  line, bubble text, animation phase, scroll) ends in that one call.
- **Do not split it back up.** If each region clears/pushes on its own, the
  three regions can be drawn in different frames → flicker / half-states
  (a status line without its alien, a bubble without its text).
- The status bar draws its two stored lines itself (`statusBar.draw()`), the
  alien and the bubble keep their own current-content state; `render()` just
  composes them. Keep that division.

## Status rule: 21 chars × 2 (pitfall 10)

Status messages must fit **21 chars/line × 2 lines** (font size 1, the top two
lines of the 128×64 panel — `STATUS_CHARS_PER_LINE`).

- Longer text goes to the **bubble** or **Serial** — **never rely on the
  SSD1306 driver clipping**. `statusBar.show()` / `statusBar.error()` truncate
  to 21 chars, so an over-long message can no longer spill into the
  alien/bubble area,
  but the *correct* fix is to shorten the message, not to lean on the cut.
- Three messages overflowed before this restructure and had to be shortened:
  `Initializing I2S bus...`, `Failed to initialize I2S bus!`,
  `Record buffer alloc failed`.
- All user-facing status strings live in `TamAIgotchi/messages.h` (one place to
  review all on-screen text). Add new ones there, not inline.

## Bubble rule

- The bubble is **always the same size (15×5)** and **always drawn**, even
  empty — no popping rectangle (issue #29 Q8).
- The bubble hosts **all content**: prompt, response, and the idle `hello`
  (`MSG_ALIEN_BUBBLE`).
- The **alien is always present** at **x 4..27 / y 42..63** (the lower-left
  quadrant), the stand frame when idle, the current animation frame while the
  idle animation runs.
- The bubble rectangle is x 30..126 / y 18..62 (`BUBBLE_X/Y/W/H` in config.h).
- The idle animation draws its bubble content **without touching the bubble's
  line table** (`bubble.renderText()`), so the stored response survives the
  animation (issue #35 step 5 follow-up, Q4). Keep that.

## Button semantics (must not drift)

| Button | Short press | Hold 5 s |
|---|---|---|
| **GPIO3** (main) | — (it is the hold button) | **hold-to-record**: hold as long as you record (max 10 s), release = send |
| **GPIO9** | scroll **down** one line | **reset WiFi settings** & reboot into the setup AP |
| **GPIO11** | scroll **up** one line | **exit the response view** back to IDLE |

- The main button (GPIO3) is **hold-to-record** — the hold is not a 5 s
  threshold action, it records for as long as it is held (capped at
  `MAX_REC_SECONDS` = 10 s) and sends the take on release.
- The two scroll buttons are only active while the response is on screen
  (RESPONSE state); during recording / sending they are ignored.
- The 5 s holds keep their meaning in **every** state (the GPIO9 WiFi reset is
  checked before the state machine branches).
- The button API is `Button::isPressed()` / `isLongPressed()` /
  `isHeld()` (`isHeld()` is new, step 5) — `update()` is called once per
  loop() pass for every button before reading them.
- Debounce 50 ms, long-press threshold 5000 ms (`BUTTON_DEBOUNCE_MS` /
  `BUTTON_LONG_PRESS_MS`) — unchanged.

## Build

- Build with **`arduino-cli`** (not the IDE), FQBN **`esp32:esp32:lolin_s2_mini`**,
  core **`esp32:esp32` 3.3.11**, from the `TamAIgotchi/` subdir:

  ```
  arduino-cli compile --fqbn esp32:esp32:lolin_s2_mini TamAIgotchi/
  ```

- **Zero warnings in project code** (library warnings are acceptable but should
  be noted).
- **Report flash/RAM sizes** vs. the previous step's baseline (expect no change,
  or a few bytes less). The post-refactor (step 10/11) baseline is
  **1 226 067 B (93%) flash / 82 812 B (25%) RAM** (step 11 is docs-only, so
  the step-11 build is identical).

## Test build

- **Every PR** uploads a `lolin-s2-mini` test build to the esp-webflasher so a
  human can flash it from the browser.
- Version naming: **`tamai-<slug>-lolin-s2-mini-<shortsha>`** (e.g.
  `tamai-cleanup-docs-lolin-s2-mini-<shortsha>`).
- Upload the **app** image (`*.ino.bin`, not `*.merged.bin`) as the firmware
  file, plus the bootloader and partition table so the addresses are inferred
  correctly.

## Module map (post-refactor, step 11/11)

The code is fully class-based (the refactoring in #42, steps 1–10). The shared
objects are constructed in `TamAIgotchi.ino` (the sketch shell: `setup()` +
`loop()` + object wiring) and referenced from the modules via the `extern` in
each header:

| Class | File | Role |
|---|---|---|
| `App` | `app.h/.cpp` | the IDLE / RECORDING / SENDING / RESPONSE state machine (formerly a global enum + the ~180 lines of inline loop logic) |
| `Display` | `display.h/.cpp` | the single render pass (`render()`) + display-level actions (`showWifiStatus()`, `resetWifiSettingsAndRestart()`, `startRecording()`) |
| `Recorder` | `recorder.h/.cpp` | the PSRAM recording buffer + the record → transcribe → LLM flow (`initRecBuffer()`, `sendRecording()`, `textGeneration()`) |
| `AlienAnimation` | `alien.h/.cpp` | the idle-alien animation state machine + rendering |
| `Bubble` | `bubble.h/.cpp` | the speech bubble: stored text, scroll, `renderText()` |
| `StatusBar` | `statusbar.h/.cpp` | the top two status lines: `show()` / `error()` / `clear()` / `draw()` (the 21-char truncation) |
| `Button` | `buttons.h/.cpp` | one instance per physical button: `update()` + `isPressed()` / `isLongPressed()` / `isHeld()` |
| `Led` | `led.h/.cpp` | the recording LED: `on()` / `off()` / `isOn()` |
| `Hardware` | `hardware.h/.cpp` | the six shared library objects (`panel` / `i2s` / `wifi` / `openai` / `chat` / `audio`) + `init()` (pins + OLED + I2S bring-up) |

Free functions that stay free: `wrapText()` in `text_utils.h/.cpp` (text
wrapping only) and `setup()` / `loop()` in `TamAIgotchi.ino` (the app shell).
All user-facing display strings live in `messages.h` (`MSG_*`).

## Layout quick reference (128×64, font size 1)

```
+--------------------------------------------------+  y=0
|  status line 1 (<=21 chars)                       |
+--------------------------------------------------+  y=8
|  status line 2 (<=21 chars)                       |
+--------------------------------------------------+  y=16
|            |  +--------------------------------+  |
|            |  |        speech bubble           |  |  x=30..126
|   alien    |  |   (prompt / response / hello)  |  |  y=18..62
|  (4..27    |  |         15 chars x 5 lines     |  |
|   42..63)  |  +--------------------------------+  |
+--------------------------------------------------+  y=64
```
