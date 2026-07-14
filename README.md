# ESP32-S3 Ethernet BOOTP/DHCP Commissioning Tool

## Overview
This firmware runs on an **ESP32-S3** equipped with a **W5500 SPI Ethernet module**. It acts as a passive network commissioning tool that listens for BOOTP and DHCP requests on a wired "field" network. 

Technicians can view discovered devices on a web-based dashboard and manually assign static IPs to them. Once an IP is assigned, the ESP32 answers the device's next BOOTP/DHCP request with the chosen IP (providing a BOOTP reply, DHCP OFFER, or DHCP ACK as appropriate).

**Current Firmware Version:** v0.14

## Hardware Requirements
* **Microcontroller:** ESP32-S3
* **Ethernet Controller:** W5500 SPI Ethernet Module

### SPI Pin Configuration (W5500 to ESP32-S3)
If the Ethernet fails to initialize, verify these connections first:
* **CS (Chip Select):** GPIO 14
* **IRQ (Interrupt):** GPIO 10
* **RST (Reset):** GPIO 9 (Use -1 in code if not connected)
* **SCK (Clock):** GPIO 13
* **MISO:** GPIO 12
* **MOSI:** GPIO 11

## Network Topology
The tool intentionally separates the control (office) network from the target (field) network to ensure clean isolation during commissioning.

1.  **Wi-Fi "Office" Side (Station Mode):**
    * Connects to a mobile hotspot (Default SSID: `Bootp`, Password: `admin1234`).
    * Hosts the web dashboard and handles all HTTP API requests (`/api/devices`, `/api/ping`, etc.).
2.  **Wired Ethernet "Field" Side (W5500):**
    * Connected to the device(s) being commissioned.
    * Listens for UDP BOOTP/DHCP requests (Server port 67, Client port 68).
    * Sends raw unicast BOOTP/DHCP replies.
    * Executes ICMP Ping requests.

## Project Structure
* `esp_bootp_v0_14_2_.ino`: The main Arduino sketch containing the setup, network routing logic, BOOTP/DHCP packet construction, and API routes.
* `webpage.h`: Contains the dashboard's HTML/CSS/JS as a PROGMEM raw string literal. It is separated from the main `.ino` file to prevent the Arduino IDE's preprocessor from misinterpreting JavaScript functions as C++ prototypes.

## Web API Endpoints
The backend provides a lightweight JSON API for the frontend:
* `GET /api/devices`: Returns the list of discovered devices (MAC, requested IP, age, etc.).
* `GET /api/settings`: Returns the tool's current Ethernet IP and Subnet Mask.
* `GET /api/set_network`: Saves a new IP/Subnet for the tool to NVS and reboots.
* `GET /api/assign`: Schedules a static IP assignment for a specific MAC address.
* `GET /api/ping`: Triggers an ICMP ping to an assigned device and waits for the result (RTT and packet loss).
* `GET /api/reboot`: Reboots the ESP32 on demand.

## Setup and Installation
1.  Open the project in the Arduino IDE.
2.  Ensure `webpage.h` is in the same directory as the `.ino` file (it will appear as a separate tab).
3.  Configure the `HOTSPOT_SSID` and `HOTSPOT_PASS` variables in the `.ino` file to match your mobile hotspot.
4.  Install the required dependencies: `WiFi`, `AsyncTCP`, `ESPAsyncWebServer`, `AsyncUDP`, `ETH`, `SPI`, `Preferences`.
5.  Compile and upload to your ESP32-S3.
