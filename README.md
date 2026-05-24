# E-Ink Gallery

Upload photos from your phone browser → they appear on the frame.

## File structure

```
eink-gallery/
├── data/
│   └── index.html          ← web UI (upload to LittleFS)
└── sketch/
    └── eink_gallery.ino    ← flash to ESP32
    (+ your Waveshare .h/.cpp files here too)
```

## Setup steps

### 1. Arduino IDE setup

Install these libraries via Library Manager:
- **ESPAsyncWebServer** by me-no-dev
- **AsyncTCP** by me-no-dev  
- **ArduinoJson** by Benoit Blanchon

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

### 6. Use it

Open http://eink-gallery.local on any device on your WiFi.

- Drop a photo onto the upload area
- Adjust contrast/sharpness — you see a live dithered preview
- Hit "send to frame"
- The frame updates on its next cycle (default 30 min)

## How it works

The browser does all the image processing (crop, dither, pack to binary)
before uploading, so the ESP32 just stores and displays raw bytes.
This keeps the ESP32 code simple and the upload small (always 15 KB).
