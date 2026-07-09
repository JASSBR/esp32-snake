# ESP32-S3 WiFi Snake Arcade

[![CI](https://github.com/JASSBR/esp32-snake/actions/workflows/ci.yml/badge.svg)](https://github.com/JASSBR/esp32-snake/actions/workflows/ci.yml)

A snake game that runs on an ESP32-S3, mirrors live gameplay to a physical
OLED screen, and is controlled from a phone or PC browser over WiFi.
Supports 1 or 2 players at once, with spectators for anyone else who joins.

## Hardware

| OLED (SSD1306 128x64) | ESP32-S3 |
|------------------------|----------|
| GND                     | GND      |
| VCC                     | 3V3      |
| SCL                     | GPIO9    |
| SDA                     | GPIO8    |

I2C address: `0x3C`. The onboard RGB LED (GPIO48) is used for status/feedback
flashes and needs no extra wiring on most ESP32-S3 dev boards.

## Features

- **Live OLED mirror** — the same game state driving the web page is drawn
  on the physical screen every tick.
- **2-player mode** — the first two connected clients each get their own
  snake (P1 filled, P2 hollow on the OLED; green vs. cyan on the web page).
  Dying on a wall, your own body, or the opponent's body ends the round;
  a head-on collision kills both. Extra clients spectate.
- **Power-up food** — ~25% of food spawns as a pulsing gold pickup worth
  +3 points instead of +1.
- **Persistent leaderboard & high score** — stored in flash (`Preferences`),
  survives reboots.
- **Runs on your home WiFi** — the board joins your existing network (station
  mode) instead of hosting its own access point, so your controlling device
  keeps its normal internet connection. The board's IP is shown on the OLED.

## Setup

### 1. WiFi credentials

Credentials are kept out of git on purpose (this repo is public).

```
cp secrets.h.example secrets.h
```

Edit `secrets.h` and fill in your real network:

```cpp
#define WIFI_SSID "your-wifi-name"
#define WIFI_PASS "your-wifi-password"
```

### 2. Arduino IDE

1. Open `esp32-snake.ino`.
2. **Tools → Board →** `ESP32S3 Dev Module`.
3. Set these board options under **Tools**:
   - USB CDC On Boot: **Disabled**
   - Partition Scheme: **Default 4MB with spiffs**
4. Install the libraries (Library Manager): `ESP Async WebServer` (ESP32Async
   fork), `Async TCP` (ESP32Async fork), `Adafruit SSD1306`, `Adafruit GFX
   Library`, `Adafruit NeoPixel`, `ArduinoJson`.
5. **Upload** the sketch.
6. Upload the `data/` folder to LittleFS — the IDE has no built-in button for
   this; install the [Arduino LittleFS Upload](https://github.com/earlephilhower/arduino-littlefs-upload)
   plugin, then use its command from the command palette.

Don't use the IDE's **Debug** (🐛) button — it requires real JTAG hardware
this board doesn't expose over USB. Use the Serial Monitor (115200 baud)
for debugging instead.

### 3. arduino-cli (alternative to the IDE)

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=default .
arduino-cli upload -p /dev/cu.usbmodemXXXX --fqbn esp32:esp32:esp32s3:CDCOnBoot=default .
```

Building and flashing the LittleFS filesystem image:

```bash
mklittlefs -c data -b 4096 -p 256 -s 0x160000 littlefs.bin
esptool --chip esp32s3 -p /dev/cu.usbmodemXXXX -b 921600 write_flash 0x290000 littlefs.bin
```

## Playing

1. Power on the board — the OLED shows its IP address once connected.
2. Open that IP in a browser (same WiFi network).
3. Controls: on-screen D-pad, arrow keys / WASD, or swipe on the canvas.
4. Open the page from a second device (or a second browser tab) to play
   2-player.

## CI

`.github/workflows/ci.yml` compiles the sketch on every push/PR to catch
build breakage early. It only verifies compilation — it doesn't flash real
hardware or build the LittleFS image.
