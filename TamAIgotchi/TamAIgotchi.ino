#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include "ESP_I2S.h"
#include <OpenAI.h>
#include "config.h"

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
I2SClass i2s;

OpenAI openai(api_key);
OpenAI_ChatCompletion chat(openai);
OpenAI_AudioTranscription audio(openai);

char ipAddress[16];
uint32_t lastButtonState = HIGH;
uint32_t lastDebounce = 0;
bool buttonPushed = false;

void combinedOutput(int x, int y, char* line, bool clrscr = false);
void displayIP();
String speechToText();
void textGeneration(String prompt);

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON1_PIN, INPUT_PULLUP);
  pinMode(BUTTON2_PIN, INPUT_PULLUP);
  pinMode(BUTTON3_PIN, INPUT_PULLUP);
  pinMode(BUTTON4_PIN, INPUT_PULLUP);
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
  combinedOutput(0, 16, "Connecting to WiFi", true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  int pos = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    combinedOutput(pos, 20, ".");
    pos = pos + 2;
    if(pos >= 128) {
      pos = 0;
    }
  }
  IPAddress ip = WiFi.localIP();
  snprintf(ipAddress, sizeof(ipAddress), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);

/* setup i2s */  
  combinedOutput(0, 16, "Initializing I2S bus...", true);
  i2s.setPins(I2S_SCK, I2S_WS, -1, I2S_DIN);
  //if (!i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
  if (!i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    combinedOutput(0, 26, "Failed to initialize I2S bus!");
    return;
  }
  combinedOutput(0, 26, "I2S bus initialized.");

/* setup openai */
  chat.setModel("gpt-4");   //Model to use for completion. Default is gpt-3.5-turbo
  chat.setSystem("Code geek");      //Description of the required assistant
  chat.setMaxTokens(1000);          //The maximum number of tokens to generate in the completion.
  chat.setTemperature(0.2);         //float between 0 and 2. Higher value gives more random results.
  chat.setStop("\r");               //Up to 4 sequences where the API will stop generating further tokens.
  chat.setPresencePenalty(0);       //float between -2.0 and 2.0. Positive values increase the model's likelihood to talk about new topics.
  chat.setFrequencyPenalty(0);      //float between -2.0 and 2.0. Positive values decrease the model's likelihood to repeat the same line verbatim.
  chat.setUser("OpenAI-ESP32");     //A unique identifier representing your end-user, which can help OpenAI to monitor and detect abuse.
}


void loop() {
  int reading = digitalRead(BUTTON1_PIN);

  if (reading != lastButtonState) {
      lastDebounce = millis();
      lastButtonState = reading;
  }

  if ((millis() - lastDebounce) > 50) {
    // Button is pushed (low due to pullup)
    if (reading == LOW) {
      // only display when button is pressed the first time
      if(!buttonPushed) {
        buttonPushed = true;
        String prompt = speechToText();
        textGeneration(prompt);
      }
    }
    else {
      // only display when button is released the first time
      if(buttonPushed) {
        buttonPushed = false;
      }
    }
  }
}

void combinedOutput(int x, int y, char* line, bool clrscr) {
  if(clrscr) {
    display.clearDisplay();
    displayIP();
  }
  Serial.println(line);
  display.setCursor(x, y);
  display.println(line);
  display.display();
}

void displayIP() {
  combinedOutput(0, 0, "IP: ");
  combinedOutput(30, 0, ipAddress);
}

String speechToText() {
  //uint8_t *wav_buffer = (uint8_t *) ps_calloc(1000, sizeof(uint8_t));
  logMemory();
  //uint8_t* wav_buffer = (uint8_t*)ps_malloc(1);
  uint8_t *wav_buffer;
  size_t wav_size;

  combinedOutput(0, 16, "Recording", true);
  digitalWrite(LED_PIN, HIGH);
  wav_buffer = i2s.recordWAV(5, &wav_size);
  digitalWrite(LED_PIN, LOW);

  logMemory();
  combinedOutput(0, 16, "Sending audio", true);
  String transcription = audio.file(wav_buffer, wav_size, OPENAI_AUDIO_INPUT_FORMAT_WAV);
  //Serial.println(transcription);

  logMemory();
  //char ctranscription[transcription.length() + 1];
  //memcpy(ctranscription, transcription.c_str(), transcription.length() + 1);
  //combinedOutput(0, 16, "Prompt: ", true);
  //combinedOutput(0, 26, ctranscription, false);

  free(wav_buffer);
  logMemory();
  return transcription;
}

void textGeneration(String prompt) {
  char cprompt[prompt.length() + 1];
  memcpy(cprompt, prompt.c_str(), prompt.length() + 1);
  combinedOutput(0, 16, "Sending prompt", true);
  combinedOutput(0, 26, cprompt, false);

  OpenAI_StringResponse result = chat.message(prompt);
  Serial.printf("Received message. Tokens: %u\n", result.tokens());
  String response = result.getAt(0);
  response.trim();
  //Serial.println(response);

  char cresponse[response.length() + 1];
  memcpy(cresponse, response.c_str(), response.length() + 1);
  combinedOutput(0, 16, "Response: ", true);
  combinedOutput(0, 26, cresponse, false);

  if(result.error()) {
    Serial.print("Error! ");
    Serial.println(result.error());
  }
}

void logMemory() {
  log_d("Used PSRAM: %d", ESP.getPsramSize() - ESP.getFreePsram());
}
