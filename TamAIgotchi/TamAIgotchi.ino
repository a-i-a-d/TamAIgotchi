#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>
#include "ESP_I2S.h"
#include <OpenAI.h>
#include "config.h"

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
I2SClass i2s;

// LocalAI endpoint + key are configurable at runtime through a small web
// setup page owned by this sketch. The values are persisted in flash (NVS)
// so they survive reboots. The constants in config.h only provide the
// initial defaults.
//
// Storage layout:
//   0  : LOCALAI_URL  (60 bytes)
//   60 : LOCALAI_KEY  (60 bytes)
#define LOCALAI_URL_MAX  60
#define LOCALAI_KEY_MAX  60
#define LOCALAI_URL_ADDR 0
#define LOCALAI_KEY_ADDR 60

WebServer localaiServer(LOCALAI_SETUP_PORT);
String localaiUrl = api_url;
String localaiKey = api_key;

// Rebuild the OpenAI client from the currently stored api_url / api_key.
// The LocalAI-ESP32 library keeps the endpoint and key for the lifetime of
// the object, so a new instance is created every time the settings change.
OpenAI openai(localaiKey.c_str(), localaiUrl.c_str());
OpenAI_ChatCompletion chat(openai);
OpenAI_AudioTranscription audio(openai);

void applyLocalAISettings() {
  // "chat" and "audio" hold a reference to "openai", so reassigning the
  // base object is enough for them to pick up the new endpoint + key.
  openai = OpenAI(localaiKey.c_str(), localaiUrl.c_str());
  Serial.print("LocalAI endpoint: ");
  Serial.println(localaiUrl);
}

uint32_t lastButtonState = HIGH;
uint32_t lastDebounce = 0;
bool buttonPushed = false;

void combinedOutput(int x, int y, const char* line, bool clrscr) {
  if(clrscr) {
    display.clearDisplay();
  }
  Serial.println(line);
  display.setCursor(x, y);
  display.println(line);
  display.display();
}

// Read the LocalAI settings from flash and fall back to the defaults
// from config.h if nothing has been saved yet.
void loadLocalAISettings() {
  EEPROM.begin(4096);
  char buf[LOCALAI_URL_MAX + 1];
  for (int i = 0; i < LOCALAI_URL_MAX; i++) {
    char c = (char)EEPROM.read(LOCALAI_URL_ADDR + i);
    if (c == '\0') break;
    buf[i] = c;
  }
  buf[LOCALAI_URL_MAX] = '\0';
  if (buf[0] != '\0') localaiUrl = String(buf);

  char kbuf[LOCALAI_KEY_MAX + 1];
  for (int i = 0; i < LOCALAI_KEY_MAX; i++) {
    char c = (char)EEPROM.read(LOCALAI_KEY_ADDR + i);
    if (c == '\0') break;
    kbuf[i] = c;
  }
  kbuf[LOCALAI_KEY_MAX] = '\0';
  if (kbuf[0] != '\0') localaiKey = String(kbuf);
  EEPROM.end();

  applyLocalAISettings();
}

// Persist the LocalAI settings into flash.
void saveLocalAISettings() {
  EEPROM.begin(4096);
  for (int i = 0; i < LOCALAI_URL_MAX; i++) {
    EEPROM.write(LOCALAI_URL_ADDR + i, localaiUrl[i]);
  }
  for (int i = 0; i < LOCALAI_KEY_MAX; i++) {
    EEPROM.write(LOCALAI_KEY_ADDR + i, localaiKey[i]);
  }
  EEPROM.commit();
  EEPROM.end();
}

// ---- LocalAI setup web page (http://<device-ip>:8081) -------------------

void handleLocalAIStatus() {
  String body = String("{\"url\":\"") + localaiUrl + F("\",\"keySet\":") + (localaiKey.length() > 0 ? "true" : "false") + F("}");
  localaiServer.send(200, "application/json", body);
}

void handleLocalAISave() {
  String url = localaiServer.hasArg("url") ? localaiServer.arg("url") : String();
  String key = localaiServer.hasArg("key") ? localaiServer.arg("key") : String();
  if (url.length() > 0 && url.length() <= LOCALAI_URL_MAX) localaiUrl = url;
  if (key.length() > 0 && key.length() <= LOCALAI_KEY_MAX) localaiKey = key;
  saveLocalAISettings();
  applyLocalAISettings();
  Serial.println("LocalAI settings saved");
  localaiServer.send(200, "text/plain", "OK");
}

void handleLocalAIPage() {
  String page = F("<html><head><title>LocalAI</title></head><body>");
  page += F("<h1>LocalAI settings</h1>");
  page += F("<form method=\"POST\" action=\"/localai/save\">");
  page += F("API URL: <input type=\"text\" name=\"url\" value=\"");
  page += localaiUrl;
  page += F("\" size=\"40\"><br>");
  page += F("API key: <input type=\"password\" name=\"key\" value=\"");
  page += localaiKey;
  page += F("\" size=\"40\"><br><br>");
  page += F("<button type=\"submit\">Save</button>");
  page += F("</form><p>Settings are stored in flash and applied immediately.</p>");
  page += F("</body></html>");
  localaiServer.send(200, "text/html", page);
}

void startLocalAIServer() {
  localaiServer.on("/localai", HTTP_GET, handleLocalAIPage);
  localaiServer.on("/localai/save", HTTP_POST, handleLocalAISave);
  localaiServer.on("/localai/status", HTTP_GET, handleLocalAIStatus);
  localaiServer.begin();
  Serial.print("LocalAI setup page: http://");
  Serial.print(WiFi.localIP().toString());
  Serial.print(":");
  Serial.println(LOCALAI_SETUP_PORT);
}

String speechToText() {
  uint8_t *wav_buffer;
  size_t wav_size;

  combinedOutput(0, 0, "Recording", true);
  digitalWrite(LED_PIN, HIGH);
  wav_buffer = i2s.recordWAV(5, &wav_size);
  digitalWrite(LED_PIN, LOW);

  combinedOutput(0, 0, "Sending audio", true);
  String transcription = audio.file(wav_buffer, wav_size, OPENAI_AUDIO_INPUT_FORMAT_WAV);
  log_d(transcription);

  free(wav_buffer);
  return transcription;
}

void textGeneration(String prompt) {
  char cprompt[prompt.length() + 1];
  memcpy(cprompt, prompt.c_str(), prompt.length() + 1);
  combinedOutput(0, 0, "Sending prompt", true);
  combinedOutput(0, 16, cprompt, false);

  OpenAI_StringResponse result = chat.message(prompt);
  Serial.printf("Received message. Tokens: %u\n", result.tokens());
  String response = result.getAt(0);
  response.trim();
  response.replace("\n", " ");
  log_d(response);

  char cresponse[response.length() + 1];
  memcpy(cresponse, response.c_str(), response.length() + 1);
  combinedOutput(0, 0, "Response: ", true);
  combinedOutput(0, 16, cresponse, false);

  if(result.error()) {
    Serial.print("Error! ");
    Serial.println(result.error());
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);

/* setup display*/
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { // Address 0x3D for 128x64
    Serial.println(F("SSD1306 allocation failed"));
    for(;;);
  }
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.clearDisplay();

/* connect to WiFi */
  combinedOutput(0, 0, "Connecting to WiFi", true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  int pos = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    combinedOutput(pos, 4, ".", false);
    pos = pos + 2;
    if(pos >= 128) {
      pos = 0;
    }
  }

/* load the LocalAI endpoint + key (flash first, config.h defaults as fallback) */
  loadLocalAISettings();

/* setup i2s */  
  combinedOutput(0, 0, "Initializing I2S bus...", true);
  i2s.setPins(I2S_SCK, I2S_WS, -1, I2S_DIN);
  if (!i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    combinedOutput(0, 16, "Failed to initialize I2S bus!", false);
    return;
  }
  combinedOutput(0, 16, "I2S bus initialized.", false);

/* setup openai */
  chat.setModel("gpt-4");           //Model to use for completion. Default is gpt-3.5-turbo
  chat.setSystem("You are communicating through a small display, keep answers as short as possible");      //Description of the required assistant
  chat.setMaxTokens(40);            //The maximum number of tokens to generate in the completion.
  chat.setTemperature(0.2);         //float between 0 and 2. Higher value gives more random results.
  chat.setStop("\r");               //Up to 4 sequences where the API will stop generating further tokens.
  chat.setPresencePenalty(0);       //float between -2.0 and 2.0. Positive values increase the model's likelihood to talk about new topics.
  chat.setFrequencyPenalty(0);      //float between -2.0 and 2.0. Positive values decrease the model's likelihood to repeat the same line verbatim.
  chat.setUser("OpenAI-ESP32");     //A unique identifier representing your end-user, which can help OpenAI to monitor and detect abuse.

  audio.setTemperature(0.1);
  audio.setLanguage("en");

/* LocalAI setup page: reachable at http://<device-ip>:8081 */
  startLocalAIServer();

/* show the final WiFi status */
  combinedOutput(0, 0, "WiFi connected", true);
  combinedOutput(0, 16, WiFi.localIP().toString().c_str(), false);
}

void loop() {
  // Keep the LocalAI setup page responsive.
  localaiServer.handleClient();

  int reading = digitalRead(BUTTON_PIN);

  if (reading != lastButtonState) {
      lastDebounce = millis();
      lastButtonState = reading;
  }

  if ((millis() - lastDebounce) > 50) {
    if (reading == LOW) { // Button is pushed (low due to pullup)
      if(!buttonPushed) {
        buttonPushed = true;
        String prompt = speechToText();
        textGeneration(prompt);
      }
    }
    else {
      if(buttonPushed) {
        buttonPushed = false;
      }
    }
  }
}
