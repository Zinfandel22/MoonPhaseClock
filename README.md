# Moon Phase Clock

An ESP32-powered round-display clock that shows the current time, date, and real-time moon phase — fetched from NASA's Dial-a-Moon API and rendered with a 30-frame moon image cycle.

This project is an adaptation of a project from [nishad2m8](https://github.com/nishad2m8) & [LazydaysCr](https://github.com/LazydaysCr)

Modified with Claude.ai to add Moon Rise/Set time

## Features

- Live time & date via NTP.
- Current moon phase name + matching moon image, pulled from NASA's Dial-a-Moon API
- Built with LVGL 8.3.11 UI designed in SquareLine Studio
- Moon rise and Moon set time displayed
- First-boot Wi-Fi, location, and time-zone setup through a local access point
- Moon images automatically rotate 180° for locations south of the equator

## Hardware

- Seeed Studio XIAO ESP32C3
- GC9A01 240×240 round SPI display

## Wiring

| GC9A01 Pin | XIAO Pin | ESP32-C3 GPIO |
|---|---|---|
| SDA / MOSI | D4 | 6 |
| SCL / SCK | D5 | 7 |
| CS | D2 | 4 |
| DC | D3 | 5 |
| RST | D1 | 3 |
| BL | Not connected (always on) | — |
| MISO | Not connected | — |

## Setup Instructions

1. **Open the project** in PlatformIO (VS Code extension).
2. **Wire the display** to the XIAO ESP32C3 per the table above. `SDA` and `SCL` on the display are used as SPI MOSI and SCK, not as I²C signals.
3. **Build and upload** using PlatformIO. The `esp32dev` environment targets the XIAO ESP32C3 and is configured for the GC9A01 display.
4. **Open the Serial Monitor** at 115200 baud and reset the board. On first boot, the clock prints the setup access point name.
5. **Connect a phone or computer** to that open access point; no password is needed. If the setup page does not open automatically, browse to `http://192.168.4.1`.
6. **Enter the network and location settings.** Use decimal latitude and longitude; a negative latitude automatically rotates the moon images 180° for the Southern Hemisphere. The time-zone field takes a POSIX TZ rule, for example `PST8PDT,M3.2.0,M11.1.0`. Leave the Wi-Fi password blank only for an open network; WPA passwords must be 8 to 63 characters.
7. **Save and connect.** The clock saves settings in ESP32 nonvolatile storage, restarts, connects to Wi-Fi, syncs time via NTP, and fetches the current moon phase.
8. **Set the MET Norway contact address** by replacing `your-email@example.com` in `src/main.cpp` before sharing or deploying the project.

The setup access point starts when no settings have been saved or when the saved Wi-Fi network cannot be reached. If connection fails, the form retains the saved location and time zone; enter the Wi-Fi password again. Saved settings do not require editing `include/credentials.h`.

The setup access point is open and does not encrypt traffic. Provision the clock only in a trusted location, then disconnect from the setup network after configuration.

## How It Works

- Time and date update every second from the ESP32's system clock (kept accurate via periodic NTP sync).
- Moon phase data is fetched from NASA's Dial-a-Moon API:
  - Every **15 seconds** until the first successful fetch.
  - Every **1 hour** afterward, since moon age changes negligibly minute-to-minute.
- The moon's age (in days) is mapped to one of 30 image frames and an 8-phase name (New Moon, Waxing Crescent, First Quarter, Waxing Gibbous, Full Moon, Waning Gibbous, Last Quarter, Waning Crescent) using evenly-spaced day thresholds.

## Notes

- If colors look swapped, toggle `TFT_RGB_ORDER` in `platformio.ini`.
- If the display appears upside down, adjust the value passed to `tft.setRotation()` in `src/main.cpp`.
