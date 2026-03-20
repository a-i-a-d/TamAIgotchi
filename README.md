# TamAIgotchi
ESP32 based client for LocalAI

This is only a small demonstration on how an ESP32 with a digital microphone and oled display can act as frontend to LocalAI.
It can be compiled and installed using the Arduino IDE

The code is the absolute minimum required.

# Required Parts

- ESP32 with PSRAM. For this project a LOLIN S2 Mini was used
- INMP441 I2S Microphone
- 0,96 Zoll OLED Display I2C 128 x 64

# Connection Diagram

# Configuration

You have to edit config.h and enter your WiFi SSID and password in the first two lines:
```
const char* ssid = "SSID";
const char* password = "PASSWORD";
```

# Required Libraries

To compile the program you'll need to install the following libraries in the Arduino IDE:
- Adafruit SSD1306

Additionally, you'll require a modified version of the OpenAI-ESP32 library that can be used with LocalAI:

__TODO: Add modified OpenAI-ESP32 installation instructions__

# Compilation and Upload

- Connect your ESP32 board via USB
- Select the correct ESP32 board in the Arduino IDE
- Click the Compile and Upload button

# Usage

When you push the button, the LED lights up and the microphone will record 5s of audio.
The audio recording is sent to your LocalAI whisper model and gets transcoded into a reply text.
The reply then is sent as prompt to the LocalAI gpt4 model and the reply is shown on the oled display.
