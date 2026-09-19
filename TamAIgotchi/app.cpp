// The app state machine (issue #53, step 10 of 11 of the refactoring plan
// in #42): the IDLE / RECORDING / SENDING / RESPONSE flow that used to be
// inline in loop() (TamAIgotchi.ino), now a class. The hidden static
// (recSecondsShown) is a member; the renderResponse() lambda is a private
// method; the recState global is state_ (owned by this class).
//
// Behavior is 1:1 with the pre-step-10 loop() (issue #53: "behavior stays
// identical") - the only changes are the object names (recState -> state_,
// displayMgr -> display_, recorder -> rec_, alien -> alien_, the buttons /
// bubble / status bar / LED via their injected references) and the statics
// -> members.
#include "app.h"
#include "hardware.h"   // Hardware (hw_ - the I2S streaming + the WiFi link)
#include "display.h"    // Display (render / showWifiStatus / startRecording)
#include "recorder.h"   // Recorder (the take streaming API + the SENDING flow)
#include "alien.h"      // AlienAnimation (markActivity + the idle-animation update)
#include "buttons.h"    // Button (isPressed / isLongPressed / isHeld / reset)
#include "bubble.h"     // Bubble (scroll / clear / lineCount / scrollOffset)
#include "statusbar.h"  // StatusBar (show / error)
#include "led.h"        // Led (on / off)
#include <Arduino.h>    // String, Serial, F(), millis()
#include "messages.h"   // MSG_* user-facing display strings (issue #36, step 6)

// issue #52, step 9 pattern: the ten dependencies are injected by
// reference - the externs are gone. The objects are all sketch-lifetime
// (defined in TamAIgotchi.ino / hardware.cpp), so the references are
// valid for the whole program.
App::App(Hardware& hw, Display& display, Recorder& rec, AlienAnimation& alien,
         Button& mainBtn, Button& scrollUp, Button& scrollDown, Bubble& bubble,
         StatusBar& status, Led& led)
    : hw_(hw), display_(display), rec_(rec), alien_(alien),
      mainBtn_(mainBtn), scrollUp_(scrollUp), scrollDown_(scrollDown),
      bubble_(bubble), status_(status), led_(led) {}

// One pass of the app state machine (issue #53, step 10): the body of
// loop() before this step, refactored. Called once per loop() pass, AFTER
// the three button update() calls (loop() is the only caller on the
// device; the host tests drive it the same way).
void App::update() {
  // Idle animation (issue #16): starts after ALIEN_IDLE_TIMEOUT_MS without
  // any button press (IDLE state) or after ALIEN_RESPONSE_TIMEOUT_MS of the
  // response being shown (RESPONSE state); any button press stops it.
  // (step 4, issue #18: the state machine lives in the AlienAnimation
  // class - see alien.h; update() starts it on the idle timeout + advances
  // the 70 s loop.)
  // issue #50, step 7 of 11 of the refactoring plan in #42: the alien no
  // longer reads the app state / recorder / WiFi library via extern - the
  // caller computes + passes the three inputs (RESPONSE flag, the recording
  // buffer allocated, and the WiFi link up).
  alien_.update(state_ == RESPONSE,
                rec_.bufferAllocated(),
                (hw_.wifi().ESP_mode != AP_MODE) && hw_.wifi().wifi_connected);

  // WiFi-config button (GPIO9): a 5 s long-press wipes the stored WiFi
  // settings and reboots into the setup AP (escape hatch for a wrong
  // password). Checked in every state, before the state machine branches.
  if (scrollDown_.isLongPressed()) {
    Serial.println(F("WiFi-config button held 5 s - resetting WiFi settings"));
    D_TDLN(F("WiFi-config button long-press: resetting WiFi settings and rebooting"));
    display_.resetWifiSettingsAndRestart(); // does not return (reboots)
  }

  // -----------------------------------------------------------------------
  // Hold-to-record state machine (issue #9):
  //   IDLE      - debounced button press starts recording (if WiFi is up and
  //               the recording buffer was allocated)
  //   RECORDING - button held: stream I2S audio into the PSRAM buffer;
  //               stops on release / buffer full / MAX_REC_SECONDS
  //   SENDING   - patch WAV header, transcribe, LLM call (blocking)
  // -----------------------------------------------------------------------

  if (state_ == IDLE) {
    if (mainBtn_.isPressed()) {  // one action per press (debounced)
      // Issue #16: a press is activity - stop the animation (if running) and
      // re-arm the inactivity timer.
      alien_.markActivity();
      // Display::startRecording() (display.h) is the deduped "start a take"
      // block (buffer / WiFi checks, beginStreaming, LED, status line); it
      // returns true if the take started. The App class owns the state
      // machine now (issue #53, step 10) - the IDLE -> RECORDING transition
      // happens here, on success (the Display module no longer sets it).
      if (display_.startRecording()) {
        state_ = RECORDING;
      }
    }
    return;
  }

  if (state_ == RECORDING) {
    // Stream I2S audio into the preallocated buffer (blocking, ~97 ms).
    // The buffer state is private (issue #49, step 6): the I2S read target
    // + the position advance go through the Recorder streaming API.
    size_t n = hw_.i2s().readBytes((char *)rec_.pcmDestination(), REC_CHUNK_BYTES);
    rec_.noteChunk(n);

    // Live recording counter (issue #33, step 3 of the UI restructure in
    // #29, Q10): status line 1 = "Recording (max 10 s)", line 2 = elapsed
    // seconds. The I2S read loop is chunked (~97 ms), so the counter
    // refreshes naturally each loop pass - throttled to once per whole
    // second to avoid re-drawing ~10x/s. (The static recSecondsShown is
    // the recSecondsShown_ member now, issue #53, step 10.)
    unsigned long recSeconds = rec_.elapsedMs() / 1000UL;
    if (recSeconds != recSecondsShown_) {
      recSecondsShown_ = recSeconds;
      status_.show(MSG_RECORDING, String(recSeconds) + " s");
    }

    // Stop conditions (checked after every chunk):
    bool stop = false;
    if (!mainBtn_.isHeld()) {
      D_TDLN(F("button released - stopping recording"));
      stop = true;
    } else if (rec_.isBufferFull()) {
      Serial.println(F("Recording buffer full - stopping"));
      D_TDLN(F("recording buffer full - stopping"));
      stop = true;
    } else if (rec_.elapsedMs() >= (unsigned long)MAX_REC_SECONDS * 1000UL) {
      Serial.println(F("Max recording time reached - stopping"));
      D_TDLN(F("max recording time reached - stopping"));
      stop = true;
    }

    if (!stop) {
      return; // still recording
    }

    // Recording finished: hand over to the send flow.
    led_.off();  // recording LED (issue #47, step 4)
    D_TD(F("recorded "));
    D_TDDEC(rec_.recordedBytes());
    D_TDLN(F(" bytes of PCM"));
    state_ = SENDING;
  }

  if (state_ == RESPONSE) {
    // Scrollable response in the speech bubble (issue #34, step 4 of 6 of
    // the UI restructure in #29): the reply lives in the bubble table;
    // the "Response x/y" counter is status line 1 (issue #29 Q6).
    // Issue #16: any button press is activity - it stops the animation
    // (if running) and restarts the auto-return timer.
    //
    // Scroll: one line per press (the double-press jump was removed in
    // issue #66 - it was annoying when scrolling fast). The 5 s holds
    // keep their meaning.

    // GPIO9 (scroll down): short press = next line; the 5 s long-press
    // (WiFi reset) is already handled above in every state.
    if (scrollDown_.isPressed()) {
      alien_.markActivity();
      bubble_.scroll(true);
      D_TD(F("scroll down ")); D_TDLN(bubble_.scrollOffset() + 1);
      renderResponse();
    }
    // GPIO11 (scroll up): short press = previous line; 5 s hold = exit the
    // response view back to IDLE.
    if (scrollUp_.isLongPressed()) {
      Serial.println(F("Scroll-up button held 5 s - exiting response view"));
      D_TDLN(F("scroll-up button long-press: back to IDLE"));
      bubble_.clear(); // Q4: the response is removed when we leave the view
      state_ = IDLE;
      display_.showWifiStatus(); // also marks activity (issue #16)
      mainBtn_.reset(); // re-arm the main button for the next press
      return;
    }
    if (scrollUp_.isPressed()) {
      alien_.markActivity();
      bubble_.scroll(false);
      D_TD(F("scroll up ")); D_TDLN(bubble_.scrollOffset() + 1);
      renderResponse();
    }
    // Main button: starts a new recording (same as in IDLE - the App class
    // owns the RESPONSE -> RECORDING transition on success, issue #53).
    if (mainBtn_.isPressed()) {
      alien_.markActivity();
      if (display_.startRecording()) { // deduped block (display.h); clears the bubble (issue #34)
        state_ = RECORDING;
      }
    }
    return;
  }

  // SENDING (blocking: transcription + LLM call). rec_.sendRecording()
  // returns true only on the full success path (transcription produced AND
  // the LLM call succeeded - textGeneration() stored the reply in the
  // bubble); on any error it returns false (the error is already shown).
  // The App class owns the state machine now (issue #53, step 10): success
  // -> RESPONSE, error -> IDLE (the main button is re-armed).
  if (rec_.sendRecording()) {
    state_ = RESPONSE;
  } else {
    state_ = IDLE;
    mainBtn_.reset(); // re-arm the debounce for the next press
    alien_.markActivity(); // issue #16: re-arm the idle-animation timer
  }
}

// Re-render the response frame (single render pass, issue #35, step 5) +
// refresh the "Response x/y" status counter. Called after every scroll,
// and on every press (a press also recovers the screen if the idle
// animation was running when it landed, issue #16: the bubble still holds
// the response text, so display_.render() restores it - Q4).
// The former renderResponse() lambda in loop() (issue #53, step 10).
void App::renderResponse() {
  display_.render();
  status_.show(MSG_RESPONSE_PREFIX + String(bubble_.scrollOffset() + 1) + "/"
             + String(bubble_.lineCount()));
}
