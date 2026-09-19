// The app state machine (issue #53, step 10 of 11 of the refactoring plan
// in #42) - the IDLE / RECORDING / SENDING / RESPONSE flow that used to be
// inline in loop() (TamAIgotchi.ino), with its hidden static
// (recSecondsShown) now a member.
//
// The App class owns the state machine:
//
//   app.update()  - one pass of the IDLE / RECORDING / SENDING / RESPONSE
//                   logic (the alien update, the 5 s WiFi-reset escape
//                   hatch, the hold-to-record flow, the response
//                   scroll logic) - call once per loop() pass,
//                   AFTER the three button update() calls
//   app.state()   - the current state (read-only; the state transitions
//                   happen inside update())
//
// The RecState enum moved here from recorder.h in this step (it is app
// state, not recorder state - issue #53).
//
// The ten dependencies (hw / display / rec / alien / the three buttons /
// bubble / status / led) are constructor-injected references (issue #52,
// step 9 pattern - the same as the Display class, issue #51, step 8).
#pragma once

// Forward declarations (complete types via the module headers in app.cpp -
// the same pattern as display.h, finding #7 in the #42 audit).
class Hardware;
class Display;
class Recorder;
class AlienAnimation;
class Button;
class Bubble;
class StatusBar;
class Led;

// App state machine (issue #9 + #13). Owned by the App class (issue #53,
// step 10 of 11 of the refactoring plan in #42 - the recState global in
// TamAIgotchi.ino is gone; declared here so app.h / app.cpp agree).
enum RecState { IDLE, RECORDING, SENDING, RESPONSE };

// The host test harness (tests/test_main.cpp / tests/test_app.cpp) needs
// to place the state machine in a state directly (the SENDING -> RESPONSE
// transition is the blocking LLM flow, not reproducible with the no-op
// host shims) + to observe the private throttle member.
struct TestHarness;
class App {
 public:
  friend struct TestHarness;
  // Construct with references to the ten shared objects (all must outlive
  // this instance - they are sketch-lifetime objects). Defined in app.cpp,
  // where the types are complete.
  App(Hardware& hw, Display& display, Recorder& rec, AlienAnimation& alien,
      Button& mainBtn, Button& scrollUp, Button& scrollDown, Bubble& bubble,
      StatusBar& status, Led& led);

  // One pass of the app state machine (issue #53, step 10): the body of
  // loop() before this step, refactored -
  //   IDLE      - a debounced main-button press starts a take (if the
  //               buffer is allocated + the WiFi is up)
  //   RECORDING - stream I2S audio into the PSRAM buffer (one chunk per
  //               pass); the live seconds counter (throttled to once per
  //               whole second); stops on release / buffer full /
  //               MAX_REC_SECONDS (in that order)
  //   SENDING   - the blocking transcription + LLM flow
  //               (rec_.sendRecording()); success -> RESPONSE, error ->
  //               IDLE (the main button is re-armed)
  //   RESPONSE  - the scroll logic (one line per press), the 5 s
  //               scroll-up hold exits to IDLE, the main button starts a
  //               new take
  // Call once per loop() pass, after the three button update() calls
  // (loop() is the only caller on the device).
  void update();

  // The current app state (read-only - the state transitions happen inside
  // update(); the recorder / display modules no longer set it, issue #53).
  RecState state() const { return state_; }

 private:
  // Re-render the response frame + refresh the "Response x/y" status
  // counter (issue #29 Q6). The former renderResponse() lambda in loop()
  // (issue #53, step 10). Called after every scroll, and on every
  // press (a press also recovers the screen if the idle animation was
  // running when it landed, issue #16: the bubble still holds the response
  // text, so display_.render() restores it - Q4).
  void renderResponse();

  // The app state machine (issue #9 + #13):
  //   IDLE      - waiting for a debounced button press
  //   RECORDING - button held: streaming I2S audio into the preallocated
  //               PSRAM buffer; LED on; stops on release / buffer full / cap
  //   SENDING   - patching the WAV header + uploading to LocalAI for
  //               transcription, then the LLM call (blocking)
  //   RESPONSE  - the LLM reply is shown in the speech bubble (issue #34,
  //               step 4 of the UI restructure in #29): GPIO9 short = scroll
  //               down, GPIO11 short = scroll up (one line per press),
  //               GPIO11 hold 5 s = back to IDLE, main button = new
  //               recording
  RecState state_ = IDLE;

  // The hidden statics from loop() (issue #53, step 10): members now.
  unsigned long recSecondsShown_ = 0;   // recording counter throttle

  // The ten shared objects (constructor-injected references, issue #52,
  // step 9 pattern).
  Hardware& hw_;
  Display& display_;
  Recorder& rec_;
  AlienAnimation& alien_;
  Button& mainBtn_;
  Button& scrollUp_;
  Button& scrollDown_;
  Bubble& bubble_;
  StatusBar& status_;
  Led& led_;
};

// The shared app object (the codebase's shared-object pattern; the instance
// is defined in TamAIgotchi.ino next to the other shared objects -
// constructed LAST, since it holds references to them, the same pattern as
// the shared `display` / `bubble` / `statusBar` objects).
extern App app;
