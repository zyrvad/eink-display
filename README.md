# E-Ink Gallery
This repository describes the hardware and software setup required to create an E-Ink display. 
Upload photos from your phone browser → they appear on the frame.

## Hardware
- Waveshare e-Paper module (this sample is using the 4.2 inch, V2 model)
- ESP32 WROOM 32
- Jumper wires

### Wiring
| Waveshare | ESP32         |
| --------- | ------------- |
| VCC       | 3.3V          |
| GND       | GND           |
| DIN       | D14           |
| CLK       | D13           |
| CS        | D15           |
| DC        | D27           |
| RST       | D26           |
| BUSY      | D25           |

## Software

### File structure

```
eink-gallery/
├── data/
│   └── index.html          ← web UI (upload to LittleFS)
└── sketch/
    └── eink_gallery.ino    ← flash to ESP32
    (+ your Waveshare .h/.cpp files here too)
```

### Setup steps

### 1. Arduino IDE setup
Install **esp32** by Espressif Systems via Board Manager (Tools -> Board Manager)

Install these libraries via Library Manager:
- **ESP Async WebServer** by ESP32Async
- **Async TCP** by ESP32Async
- **ArduinoJson** by Benoit Blanchon

Follow the official documentation for Waveshare and install the library from the following link: https://www.waveshare.com/wiki/E-Paper_ESP32_Driver_Board
- Once installed the unzipped file `esp32-waveshare-epd` should be moved under ~/Arduino/libraries

More general documentation for different microcontrollers can be found here: https://www.waveshare.com/wiki/4.2inch_e-Paper_Module_Manual?srsltid=AfmBOorC3uJt6WIOzAR03IcbcNYY6qKfUrVzAfDpQm8CcyQrHxyf6_mp#ESP32

### 2. Partition scheme

Tools → Partition Scheme → **"Default 4MB with spiffs"**
(LittleFS uses this same partition)

### 3. Install LittleFS upload plugin

Download from: https://github.com/lorol/arduino-esp32littlefs-plugin
Place the .jar in: `~/Documents/Arduino/tools/ESP32LittleFS/tool/`
Restart Arduino IDE.

### 4. Flash the sketch

Edit the config at the top of eink_gallery.ino:
```cpp
const char* WIFI_SSID     = "your_network";
const char* WIFI_PASSWORD = "your_password";
const int   DISPLAY_MINS  = 30;
```
Upload normally (Ctrl+U).

### 5. Upload the web UI to LittleFS

Copy `data/index.html` into your sketch folder under a `data/` subfolder.
Then: Tools → **ESP32 LittleFS Data Upload**

**TO_FIX:** Plugin not really working so HTML is directly embedded in sketch

### 6. Use it

Power the ESP32 board via USB-C cable.

Open http://192.168.1.82 on any device on your WiFi.

- Drop a photo onto the upload area
- Adjust contrast/sharpness — you see a live dithered preview
- Hit "send to frame"
- The frame updates on its next cycle

## How it works

The browser does all the image processing (crop, dither, pack to binary)
before uploading, so the ESP32 just stores and displays raw bytes.
This keeps the ESP32 code simple and the upload small (always 15 KB).
