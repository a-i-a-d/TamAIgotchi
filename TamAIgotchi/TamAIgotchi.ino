// TamAIgotchi - ESP32 based client for LocalAI (app shell).
//
// Step 10 of the refactoring proposed in issue #42: this file now only
// contains setup() + loop() (loop() is the object wiring + the ESP-Wifi-
// Config machinery + the button debounce edge-detect + app.update()).
// Everything else lives in dedicated modules:
//   hardware.h   - hw (the Hardware class, issue #52, step 9: owns the six
//                  shared library objects panel/i2s/wifi/openai/chat/audio
//                  + init() (pinMode + OLED + I2S bring-up))
//   display.h    - displayMgr (the Display class: render() / showWifiStatus() /
//                  resetWifiSettingsAndRestart() / startRecording(), issue #51, step 8)
//   app.h        - app (the App class, issue #53, step 10: owns the
//                  IDLE/RECORDING/SENDING/RESPONSE state machine, formerly
//                  the recState global + the ~180 lines of inline logic in
//                  loop(); the RecState enum moved here from recorder.h)
//   recorder.h   - PSRAM recording buffer + SENDING/RESPONSE flow
//   alien.h      - the idle-alien animation (issue #16)
//   buttons.h    - the debounced buttons (step 2)
//   text_utils.h - wrapText() (step 1; the display-role helpers were
//                  removed during the UI restructure in #29, issues #33/#36)
//   statusbar.h  - statusBar (show / error / clear / draw, step 3,
//                  issue #46: the top two lines are the status bar)
#include "hardware.h"   // hw (the Hardware class, issue #52, step 9)
#include "display.h"    // displayMgr (the Display class, issue #51, step 8)
#include "app.h"        // app (the App class, issue #53, step 10) + RecState
#include "buttons.h"    // Button instances
#include "recorder.h"   // Recorder
#include "alien.h"      // AlienAnimation
#include "statusbar.h"  // statusBar (issue #46, step 3)
#include "messages.h"   // MSG_* user-facing display strings (issue #36, step 6)
#include "bubble.h"     // bubble (scroll / render, issue #34, step 4)
#include "led.h"      // led (recording LED, issue #47, step 4)

// Hardware (issue #52, step 9 of 11 of the refactoring in #42): owns the
// six shared library objects (panel / i2s / wifi / openai / chat / audio,
// formerly the globals in hardware.h) + the bring-up (hw.init(), formerly
// hardwareInit()). Constructed FIRST - every other shared object below
// references its members (the same shared-object pattern as `bubble` /
// `statusBar` / `led`: the instance lives in the sketch, the modules
// reference it via the extern in hardware.h).
Hardware hw;

// Buttons (step 2 of the refactoring, issue #18): one Button instance per
// physical button. update() is called once per loop() pass for every
// button before reading isPressed() / isLongPressed() (see buttons.h).
Button mainBtn(BUTTON_PIN);
Button scrollUpBtn(SCROLL_UP_PIN);
Button scrollDownBtn(WIFI_CONFIG_BUTTON_PIN);

// Recorder (step 3 of the refactoring, issue #18): owns the preallocated
// PSRAM recording buffer + the SENDING/RESPONSE flow (recorder.initRecBuffer /
// recorder.sendRecording / textGeneration). See recorder.h.
// issue #52, step 9: the OpenAI clients are constructor-injected references
// (hw.chat() / hw.audio()) - the `extern OpenAI_*` in recorder.cpp is gone.
Recorder recorder(hw.chat(), hw.audio());

// Alien (step 4 of the refactoring, issue #18): owns the idle-animation state
// machine + rendering (markActivity() on button events, update() each pass).
// See alien.h. issue #52, step 9: the panel is a constructor-injected
// reference (hw.panel()) - the `extern Adafruit_SSD1306` in alien.cpp is gone.
AlienAnimation alien(hw.panel());

// Bubble (step 2 of 11 of the refactoring in #42, issue #45): the shared
// speech-bubble object (the codebase's existing shared-object pattern -
// the instance lives in the sketch, the modules reference it via the
// extern in bubble.h, same as `display`). issue #52, step 9: the panel is a
// constructor-injected reference (hw.panel()) - the `extern Adafruit_SSD1306`
// in bubble.cpp is gone.
Bubble bubble(hw.panel());

// Status bar (step 3 of 11 of the refactoring in #42, issue #46): the
// shared status-bar object (same shared-object pattern as `bubble` - the
// instance lives in the sketch, the modules reference it via the extern
// in statusbar.h). issue #52, step 9: the panel is a constructor-injected
// reference (hw.panel()) - the `extern Adafruit_SSD1306` in statusbar.cpp is
// gone.
StatusBar statusBar(hw.panel());

// Recording LED (step 4 of 11 of the refactoring in #42, issue #47): the
// shared LED object (same shared-object pattern as `bubble` / `statusBar`
// - the instance lives in the sketch, the modules reference it via the
// extern in led.h).
Led led(LED_PIN);

// Display manager (step 8 of 11 of the refactoring in #42, issue #51): the
// shared display object owning the single render pass + the display-level
// actions (showWifiStatus / resetWifiSettingsAndRestart / startRecording).
// Constructed AFTER the six objects it references (display / statusBar /
// alien / bubble / wifiConfig / recorder / led) - the same shared-object
// pattern as `bubble` / `statusBar` / `led` (the instance lives in the
// sketch, the modules reference it via the extern in display.h).
Display displayMgr(hw.panel(), statusBar, alien, bubble, hw.wifi(), recorder, led);

// App (step 10 of 11 of the refactoring in #42, issue #53): the app state
// machine (IDLE / RECORDING / SENDING / RESPONSE) that used to be the
// recState global + the ~180 lines of inline logic in loop(). Constructed
// LAST - it references every shared object above (the same shared-object
// pattern as `displayMgr` - the instance lives in the sketch, the modules
// reference it via the extern in app.h).
App app(hw, displayMgr, recorder, alien, mainBtn, scrollUpBtn, scrollDownBtn,
        bubble, statusBar, led);

void setup() {
  Serial.begin(115200);
  D_TDLN(F("setup() start"));

  // Hardware bring-up (step 5 of the refactoring, issue #18): pinMode +
  // OLED init + I2S init, in Hardware::init() (issue #52, step 9: the
  // inline hardwareInit() is a method of the Hardware class).
  if (!hw.init()) {
    return; // I2S failed to initialize - the error is already on the display
  }
  D_TDLN(F("hardware init done (pins, OLED, I2S)"));

  // Register the LocalAI user settings BEFORE initialize() (library API
  // requirement). The config.h values are the initial defaults.
  if (hw.wifi().addSetting("LOCALAI_URL", api_url) < 0)
    Serial.println(F("WARNING: could not register LOCALAI_URL setting"));
  if (hw.wifi().addSetting("LOCALAI_KEY", api_key) < 0)
    Serial.println(F("WARNING: could not register LOCALAI_KEY setting"));
  D_TDLN(F("LocalAI settings registered (LOCALAI_URL, LOCALAI_KEY)"));

/* connect to WiFi (or start the setup access point) */
  statusBar.show(MSG_WIFI_CONNECTING);
  if (hw.wifi().initialize() == AP_MODE) {
    // No known network was reachable: the device is broadcasting an access
    // point. Keep the setup web server running so the WiFi can be configured.
    hw.wifi().Start_HTTP_Server(0);
  }

  D_TD(F("WiFi mode after initialize(): "));
  D_TDLN(hw.wifi().ESP_mode == AP_MODE ? "AP (setup page)" : "STA");

/* allocate the hold-to-record buffer once (PSRAM), before the OpenAI client
   so the upload buffer is sized with the recording buffer already in place */
  if (!recorder.initRecBuffer()) {
    // No fallback to the old fixed 5 s recording: the error is already on
    // the display. Recording stays disabled until the device is rebooted
    // with enough free memory. (issue #53, step 10: the app state is owned
    // by the App class - it starts at IDLE by default, so no assignment.)
  }

/* setup openai (endpoint + key from the stored settings: Custom tab of the
   setup page, config.h defaults on first boot / after a full reset) */
  String localaiUrl = hw.wifi().getSetting("LOCALAI_URL");
  String localaiKey = hw.wifi().getSetting("LOCALAI_KEY");
  if (localaiUrl.length() == 0) localaiUrl = api_url;
  if (localaiKey.length() == 0) localaiKey = api_key;
  // issue #52, step 9: the client (re)build is a Hardware method (was
  // `openai = OpenAI(...)`); the chat/audio clients reference the same
  // object, so they pick up the new endpoint/key automatically.
  hw.setOpenAI(localaiUrl.c_str(), localaiKey.c_str());
  Serial.print(F("LocalAI endpoint: "));
  Serial.println(localaiUrl);
  D_TD(F("LocalAI endpoint resolved: "));
  D_TDLN(localaiUrl);

  hw.chat().setModel("gpt-4");           //Model to use for completion. Default is gpt-3.5-turbo
  D_TDLN(F("chat model: gpt-4, max_tokens: 200, temperature: 0.2"));
  hw.chat().setSystem("You are communicating through a small display, keep answers as short as possible");      //Description of the required assistant
  hw.chat().setMaxTokens(LLM_MAX_TOKENS); //The maximum number of tokens to generate (issue #35, step 5 of #29: 40 -> 200 via config.h).
  hw.chat().setTemperature(0.2);         //float between 0 and 2. Higher value gives more random results.
  hw.chat().setStop("\r");               //Up to 4 sequences where the API will stop generating further tokens.
  hw.chat().setPresencePenalty(0);       //float between -2.0 and 2.0. Positive values increase the model's likelihood to talk about new topics.
  hw.chat().setFrequencyPenalty(0);      //float between -2.0 and 2.0. Positive values decrease the model's likelihood to repeat the same line verbatim.
  hw.chat().setUser("OpenAI-ESP32");     //A unique identifier representing your end-user, which can help OpenAI to monitor and detect abuse.

  hw.audio().setTemperature(0.1);
  hw.audio().setLanguage("en");

/* show the final WiFi status (access point name + IP, or the assigned IP) */
  displayMgr.showWifiStatus();
  D_TDLN(F("setup() done"));
}

void loop() {
  // Keep the ESP-Wifi-Config machinery running: it (re)connects to a known
  // network and serves the setup page while in AP mode.
  hw.wifi().handle(10000);

  // Debounce edge-detect for all buttons (step 2, issue #18): call once
  // per loop() pass for every button, before reading isPressed() /
  // isLongPressed().
  mainBtn.update();
  scrollUpBtn.update();
  scrollDownBtn.update();

  // The app state machine (issue #53, step 10 of 11 of the refactoring in
  // #42): the IDLE / RECORDING / SENDING / RESPONSE flow, the alien idle
  // animation update, the 5 s WiFi-reset escape hatch and the response
  // scroll logic all live in App::update() now (app.cpp).
  app.update();
}
