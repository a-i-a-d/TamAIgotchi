// Tests for the App class (issue #53, step 10 of 11 of the refactoring
// plan in #42) - the app state machine that used to be inline in loop()
// (TamAIgotchi.ino), now testable.
//
// Pins the behavior of the IDLE / RECORDING / SENDING / RESPONSE flow
// (issue #53 test list):
//   - IDLE -> RECORDING (main button press, WiFi connected, buffer
//     allocated - the Display::startRecording() step-8 block)
//   - RECORDING -> SENDING (release / buffer full / max time, in order)
//   - SENDING -> IDLE (the host shims return an empty transcription ->
//     the error path; the main button is re-armed)
//   - SENDING -> RESPONSE (the textGeneration() success path - driven
//     through the shared recorder, the device's own code)
//   - RESPONSE -> IDLE (scroll-up 5 s hold)
//   - RESPONSE scroll: down/up single press (offset changes)
//   - the recSecondsShown_ throttle (the recording counter refreshes once
//     per whole second)
//
// The shared `app` object is defined in tests/test_main.cpp (the host
// build's equivalent of the instance in TamAIgotchi.ino). The buttons are
// driven through the host pin table (the Button reads host_pin_level[]) +
// the shim clock (host_set_millis()), exactly like the Button tests.
#include "test_main.h"
#include <Arduino.h>        // host shim: host_set_millis / host_set_pin
#include <ESPWifiConfig.h>  // ESPWifiConfig + STA_MODE / AP_MODE
#include <esp_heap_caps.h>  // host shim: host_set_heap_free
#include "app.h"            // App + RecState (issue #53, step 10)
#include "hardware.h"       // hw (the shared Hardware object, issue #52, step 9)
#include "buttons.h"        // the shared Button instances
#include "bubble.h"         // Bubble (the response text table)
#include "statusbar.h"      // StatusBar (the "Response x/y" counter)
#include "display.h"        // Display (render passes)
#include "recorder.h"       // Recorder (the shared recorder object)
#include "led.h"            // Led (the shared led object)
#include "config.h"         // BUTTON_PIN / SCROLL_*_PIN, REC_CHUNK_BYTES, BUBBLE_VISIBLE_LINES
#include "messages.h"       // MSG_RECORDING, MSG_RESPONSE_PREFIX

// The shared objects (defined in tests/test_main.cpp for the host build).
extern App app;
extern Hardware hw;
extern Bubble bubble;
extern StatusBar statusBar;
extern Display displayMgr;
extern Recorder recorder;
extern Led led;

// The three shared button objects (defined in tests/test_main.cpp).
extern Button mainBtn;
extern Button scrollUpBtn;
extern Button scrollDownBtn;

// The test harness (friend of App - issue #53, step 10): places the state
// machine directly (the SENDING -> RESPONSE transition is the blocking LLM
// flow, not reproducible with the no-op host shims) + observes the private
// throttle member.
struct TestHarness {
  static RecState state() { return app.state_; }
  static void set_state(RecState s) { app.state_ = s; }
  static unsigned long recSecondsShown() { return app.recSecondsShown_; }
  static void reset_recSecondsShown() { app.recSecondsShown_ = 0; }
};

// --- helpers ----------------------------------------------------------------

// One pass of the device loop(): the button debounce edge-detect for every
// button, then the app state machine (the same order as TamAIgotchi.ino).
// The button update() calls read the host pin table + the shim clock, so
// the buttons behave exactly like on the device (50 ms debounce, 5 s
// long-press).
static void loop_pass() {
  mainBtn.update();
  scrollUpBtn.update();
  scrollDownBtn.update();
  app.update();
}

// Reset the shared test state to a known baseline: the clock at 0, every
// button released (HIGH), the app state at IDLE, the bubble empty, the
// throttle member at its fresh-instance value.
static void reset_app_state() {
  host_set_millis(0);
  host_set_pin(BUTTON_PIN, HIGH);
  host_set_pin(SCROLL_UP_PIN, HIGH);
  host_set_pin(WIFI_CONFIG_BUTTON_PIN, HIGH);
  mainBtn.reset();
  scrollUpBtn.reset();
  scrollDownBtn.reset();
  bubble.clear();
  led.off();
  TestHarness::set_state(IDLE);
  TestHarness::reset_recSecondsShown();
  // The OpenAI shim hooks are empty by default (the no-op error path).
  host_openai_transcription = nullptr;
  host_openai_chat_response = nullptr;
  host_openai_chat_error = nullptr;
  // The device boots with the recording buffer allocated (setup()); the
  // shared recorder re-inits idempotently (1 MB of "PSRAM" is enough).
  host_set_heap_free(1024 * 1024);
  recorder.initRecBuffer();
  hw.wifi().ESP_mode = STA_MODE;
  hw.wifi().wifi_connected = true;
}

// Drive a button through the debounce window (the press is "consumed" by
// the next loop_pass() - one action per press, like the device).
static void press_button(Button& btn, int pin, unsigned long at_ms) {
  host_set_millis(at_ms);
  host_set_pin(pin, LOW);
  btn.update();  // sees the press edge (pressStart = at_ms)
  host_set_millis(at_ms + BUTTON_DEBOUNCE_MS + 1);
  btn.update();  // held past the debounce window -> isPressed() true now
}

// Hold a button past the 5 s long-press threshold (the long-press fires on
// the next loop_pass()).
static void hold_button(Button& btn, int pin, unsigned long at_ms) {
  host_set_millis(at_ms);
  host_set_pin(pin, LOW);
  btn.update();
  host_set_millis(at_ms + BUTTON_LONG_PRESS_MS);
  btn.update();
}

// Release a button (re-arms the debounce on the HIGH edge).
static void release_button(Button& btn, int pin, unsigned long at_ms) {
  host_set_millis(at_ms);
  host_set_pin(pin, HIGH);
  btn.update();
}

// A response long enough to scroll: BUBBLE_VISIBLE_LINES + 10 wrapped
// lines (each "wordN" is 5 chars, well under the 15-char line width).
static String long_response() {
  String r;
  for (int i = 0; i < BUBBLE_VISIBLE_LINES + 10; i++) {
    if (i) r += " ";
    r += "word" + String(i);
  }
  return r;
}


// --- IDLE -> RECORDING (issue #53 test list, item 1) -------------------------

TEST(app_idle_press_starts_recording) {
  reset_app_state();
  CHECK(TestHarness::state() == IDLE);
  CHECK(recorder.bufferAllocated());       // the buffer was allocated
  CHECK(hw.wifi().wifi_connected);         // the WiFi is connected

  // A debounced press of the main button in IDLE starts a take:
  // Display::startRecording() (the step-8 block) runs - the take position
  // is reset, the LED is on, the "Recording (max 10 s)" status line is
  // shown, and the app state is RECORDING (the App class owns it now,
  // issue #53, step 10 - the state transition happens in the IDLE branch
  // of App::update()).
  press_button(mainBtn, BUTTON_PIN, 100);
  loop_pass();

  CHECK(led.isOn());                        // the recording LED is on
  CHECK_EQ(statusBar.line1().c_str(), MSG_RECORDING); // "Recording (max 10 s)"
  CHECK_EQ_INT(0, (long)recorder.recordedBytes()); // beginStreaming() reset
}

// IDLE with the WiFi NOT connected: the press does not start a take
// (Display::startRecording() shows the AP / connection status instead -
// the step-8 behavior, now reachable through the App class).
TEST(app_idle_press_wifi_down_does_not_start) {
  reset_app_state();
  hw.wifi().wifi_connected = false;

  press_button(mainBtn, BUTTON_PIN, 100);
  loop_pass();

  CHECK(!led.isOn());  // no take started
  CHECK_EQ(statusBar.line1().c_str(), MSG_WIFI_CONNECTING); // the status screen
}

// --- RECORDING: one chunk per pass + the counter throttle --------------------

TEST(app_recording_streams_one_chunk_per_pass) {
  reset_app_state();
  press_button(mainBtn, BUTTON_PIN, 100);
  loop_pass();  // IDLE -> start the take (LED on)
  CHECK(led.isOn());

  // RECORDING: one I2S read per pass (the fake I2S returns REC_CHUNK_BYTES,
  // so the position advances by exactly that much per pass - the streaming
  // API of issue #49, step 6).
  size_t before = recorder.recordedBytes();
  host_set_millis(300);
  loop_pass();
  CHECK_EQ_INT((long)before + REC_CHUNK_BYTES, (long)recorder.recordedBytes());

  host_set_millis(400);
  loop_pass();
  CHECK_EQ_INT((long)before + 2 * REC_CHUNK_BYTES, (long)recorder.recordedBytes());
}

TEST(app_recording_counter_throttles_to_whole_seconds) {
  reset_app_state();
  press_button(mainBtn, BUTTON_PIN, 100);
  loop_pass();  // IDLE -> RECORDING (the take started; rec_start stamped)

  // The live counter (status line 2 = "<n> s") refreshes when the whole
  // second changes (recSecondsShown_ - the static recSecondsShown is a
  // member now, issue #53, step 10). The throttle starts at 0, so second
  // 0 is "already shown" (no render pass); from second 1 on, each whole
  // second refreshes the counter exactly once (the pre-step-10 behavior:
  // the counter first appears as "1 s").
  hw.panel().reset();
  host_set_millis(500);   // second 0 (elapsed < 1 s from the take start)
  loop_pass();
  CHECK_EQ_INT(0, hw.panel().frames);  // throttled (0 == recSecondsShown_)

  host_set_millis(900);   // still second 0
  loop_pass();
  CHECK_EQ_INT(0, hw.panel().frames);  // no re-show

  // Second 1: the counter refreshes exactly once.
  host_set_millis(1500);
  loop_pass();
  CHECK_EQ(statusBar.line2().c_str(), "1 s");
  CHECK_EQ_INT(1, hw.panel().frames);  // one render pass for "1 s"

  // Still in second 1: throttled again.
  host_set_millis(1900);
  loop_pass();
  CHECK_EQ_INT(1, hw.panel().frames);

  // Second 2: refreshes exactly once more.
  host_set_millis(2500);
  loop_pass();
  CHECK_EQ(statusBar.line2().c_str(), "2 s");
  CHECK_EQ_INT(2, hw.panel().frames);

  // Still in second 2: throttled.
  host_set_millis(2900);
  loop_pass();
  CHECK_EQ_INT(2, hw.panel().frames);
}

// --- RECORDING -> SENDING: the stop conditions (in order) --------------------

TEST(app_recording_release_stops_and_sends) {
  reset_app_state();
  press_button(mainBtn, BUTTON_PIN, 100);
  loop_pass();  // take started
  CHECK(led.isOn());

  // Release the main button: the RECORDING branch stops (release is the
  // FIRST stop condition) and hands over to SENDING (the LED is off).
  // The SENDING flow runs in the SAME pass (the fall-through is the
  // pre-step-10 behavior, issue #53: behavior stays identical): the host
  // shims return an empty transcription (the error path - the network is
  // NOT emulated) -> back to IDLE (issue #53 test list, item 3:
  // SENDING -> IDLE on error), the main button re-armed.
  release_button(mainBtn, BUTTON_PIN, 400);
  host_set_millis(500);
  loop_pass();

  CHECK(!led.isOn());
  CHECK(TestHarness::state() == IDLE);
}

TEST(app_recording_buffer_full_stops_and_sends) {
  reset_app_state();
  press_button(mainBtn, BUTTON_PIN, 100);
  loop_pass();  // take started

  // Fill the buffer to capacity (the second stop condition - the button
  // stays held, so the release check does not fire first).
  size_t cap = (size_t)MAX_REC_SECONDS * 65536;
  recorder.noteChunk(cap);
  CHECK(recorder.isBufferFull());

  host_set_millis(300);
  loop_pass();

  CHECK(!led.isOn());
  // The SENDING flow ran in the same pass (the fall-through): the host
  // shims return an empty transcription (the error path) -> IDLE.
  CHECK(TestHarness::state() == IDLE);
}

TEST(app_recording_max_time_stops_and_sends) {
  reset_app_state();
  press_button(mainBtn, BUTTON_PIN, 100);
  loop_pass();  // take started (beginStreaming() stamped the clock at ~100 ms)

  // Pass the MAX_REC_SECONDS cap (the third stop condition - the button
  // stays held + the buffer is not full). The take started at ~151 ms
  // (beginStreaming() stamped the clock in the IDLE pass), so use a
  // margin past the cap.
  host_set_millis(151UL + (unsigned long)MAX_REC_SECONDS * 1000UL + 1000UL);
  loop_pass();

  CHECK(!led.isOn());
  // The SENDING flow ran in the same pass (the fall-through): the host
  // shims return an empty transcription (the error path) -> IDLE.
  CHECK(TestHarness::state() == IDLE);
}

// --- SENDING -> RESPONSE (the success path, driven through the recorder) -----

TEST(app_sending_success_reaches_response) {
  reset_app_state();
  // The device path: the RECORDING branch hands over to SENDING, then
  // rec_.sendRecording() runs the transcription + textGeneration() (the
  // shared recorder - the device's own code), which stores the reply in
  // the bubble. The OpenAI shim hooks (issue #53, step 10) drive the
  // success path on the host: a non-empty transcription + a non-empty
  // LLM reply.
  host_openai_transcription = "hello alien";
  host_openai_chat_response = "I am the alien!";
  TestHarness::set_state(SENDING);

  loop_pass();

  CHECK(TestHarness::state() == RESPONSE);  // SENDING -> RESPONSE (success)
  CHECK(bubble.lineCount() > 0);            // the reply is in the bubble
  CHECK_EQ(statusBar.line1().c_str(),
           (String(MSG_RESPONSE_PREFIX) + String(1) + "/" + String(bubble.lineCount())).c_str());
}

// --- RESPONSE -> IDLE (the 5 s scroll-up hold, issue #53 test list) ----------

TEST(app_response_hold_up_five_s_exits_to_idle) {
  reset_app_state();
  bubble.setText(long_response());
  TestHarness::set_state(RESPONSE);

  // Hold the scroll-up button past the 5 s threshold: the RESPONSE branch
  // exits to IDLE (the bubble is cleared, the WiFi status is shown, the
  // main button is re-armed).
  hold_button(scrollUpBtn, SCROLL_UP_PIN, 1000);
  loop_pass();

  CHECK(TestHarness::state() == IDLE);
  CHECK_EQ_INT(0, bubble.lineCount());  // the response is removed (Q4)
}

// --- RESPONSE scroll: single press (offset changes) ---------------------------

TEST(app_response_scroll_down_single_press) {
  reset_app_state();
  bubble.setText(long_response());
  CHECK(bubble.lineCount() > BUBBLE_VISIBLE_LINES);
  CHECK_EQ_INT(0, bubble.scrollOffset());
  TestHarness::set_state(RESPONSE);

  // A single scroll-down press moves the offset to 1 + refreshes the
  // "Response 2/N" counter. (The button is released after each press -
  // like on the device, the one-shot flag re-arms on the HIGH edge.)
  press_button(scrollDownBtn, WIFI_CONFIG_BUTTON_PIN, 1000);
  loop_pass();
  release_button(scrollDownBtn, WIFI_CONFIG_BUTTON_PIN, 1100);

  CHECK_EQ_INT(1, bubble.scrollOffset());
  int n = bubble.lineCount();
  CHECK_EQ(statusBar.line1().c_str(), (String(MSG_RESPONSE_PREFIX) + String(2) + "/" + String(n)).c_str());
}

TEST(app_response_scroll_up_single_press) {
  reset_app_state();
  bubble.setText(long_response());
  TestHarness::set_state(RESPONSE);

  // Move to the middle first (two downs), then one up: offset 1.
  press_button(scrollDownBtn, WIFI_CONFIG_BUTTON_PIN, 1000);
  loop_pass();
  release_button(scrollDownBtn, WIFI_CONFIG_BUTTON_PIN, 1100);
  CHECK_EQ_INT(1, bubble.scrollOffset());
  press_button(scrollDownBtn, WIFI_CONFIG_BUTTON_PIN, 2000);
  loop_pass();
  release_button(scrollDownBtn, WIFI_CONFIG_BUTTON_PIN, 2100);
  CHECK_EQ_INT(2, bubble.scrollOffset());

  press_button(scrollUpBtn, SCROLL_UP_PIN, 3000);
  loop_pass();
  release_button(scrollUpBtn, SCROLL_UP_PIN, 3100);
  CHECK_EQ_INT(1, bubble.scrollOffset());
}

// --- RESPONSE: the main button starts a new take ------------------------------

TEST(app_response_main_button_starts_new_take) {
  reset_app_state();
  bubble.setText(long_response());
  TestHarness::set_state(RESPONSE);

  // The main button in RESPONSE starts a new recording (same as in IDLE -
  // Display::startRecording() clears the bubble, issue #34).
  press_button(mainBtn, BUTTON_PIN, 1000);
  loop_pass();
  release_button(mainBtn, BUTTON_PIN, 1100);

  CHECK(led.isOn());
  CHECK_EQ_INT(0, bubble.lineCount());  // the bubble was cleared
  CHECK_EQ(statusBar.line1().c_str(), MSG_RECORDING);
}
