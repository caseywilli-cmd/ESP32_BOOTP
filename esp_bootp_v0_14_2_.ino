// ==========================================================================
// ESP32-S3 Ethernet BOOTP/DHCP Commissioning Tool
// ==========================================================================
// PURPOSE
//   Runs on an ESP32-S3 with a W5500 SPI Ethernet module. It passively
//   listens for BOOTP/DHCP requests on the wired "field" network, lists the
//   devices it hears from, and lets a technician assign each one a static
//   IP by hand from a web dashboard. When the technician clicks "Assign",
//   the next BOOTP/DHCP request from that device is answered with the
//   chosen IP (a BOOTP reply, DHCP OFFER, or DHCP ACK, depending on what
//   the device asked for).
//
// NETWORK TOPOLOGY (important for troubleshooting!)
//   - Wi-Fi (station mode) connects to a mobile hotspot and serves the web
//     dashboard (loadData/loadSettings/ping/etc). This is the "office" side.
//   - Wired Ethernet (W5500 over SPI) is the "field" side: this is where
//     BOOTP/DHCP requests are heard and answered, and where /api/ping
//     packets are sent from. The two networks are intentionally separate.
//   - Because both interfaces are up at once, outbound traffic needs to be
//     explicitly told which interface to use where it matters (see the
//     ETH_CONNECTED handler in setup() and the /api/ping handler below).
//
// FIRMWARE VERSION: v0.14 (this revision adds inline documentation and a
//   dashboard UI refresh; no protocol-level behavior was changed).
// ==========================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncUDP.h>
#include <ETH.h>
#include <SPI.h>
#include <Preferences.h>
#include "esp_netif.h"
#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"

// ==========================================
// USER CONFIGURATION
// ==========================================
// Credentials for the Wi-Fi hotspot the ESP32 joins to serve the web UI.
// This is NOT the field network the devices being commissioned live on.
const char* HOTSPOT_SSID = "Bootp";
const char* HOTSPOT_PASS = "admin1234";

// SPI Pins for W5500
// These map the W5500 Ethernet controller's SPI + control lines to ESP32-S3
// GPIOs. If Ethernet fails to initialize, verify these against the wiring
// first, since a lot of "server won't start" reports trace back to this.
#define ETH_PHY_TYPE ETH_PHY_W5500
#define ETH_PHY_ADDR  1
#define ETH_PHY_CS    14  // Chip Select
#define ETH_PHY_IRQ   10  // Interrupt Request
#define ETH_PHY_RST   9   // Reset (Use -1 if not connected)
#define ETH_SPI_SCK   13
#define ETH_SPI_MISO  12
#define ETH_SPI_MOSI  11

// Standard BOOTP/DHCP UDP ports. Server (this device) listens on 67;
// clients (the devices being commissioned) listen on 68.
#define BOOTP_SERVER_PORT 67
#define BOOTP_CLIENT_PORT 68

// ==========================================
// GLOBALS & DATA STRUCTURES
// ==========================================
AsyncWebServer server(80);   // Serves the dashboard + JSON API on Wi-Fi
AsyncUDP udp;                 // Listens for BOOTP/DHCP requests on Ethernet
Preferences prefs;            // NVS storage for the tool's own static IP/mask
SPIClass spi(FSPI);           // SPI bus used to talk to the W5500

// The tool's own Ethernet IP/subnet (persisted in flash via Preferences).
// These are what the technician sets in the "Tool Network Settings" card.
String current_eth_ip = "192.168.1.100";
String current_eth_sn = "255.255.255.0";

// Set by /api/set_network and /api/reboot; loop() watches this and performs
// a delayed restart so the HTTP response has time to actually reach the
// browser before the Wi-Fi radio disappears mid-reply.
bool shouldReboot = false;
unsigned long rebootTimer = 0;

// One entry per device the tool has heard a BOOTP/DHCP request from.
// This is a fixed-size table (no heap allocation) since it only ever needs
// to hold a handful of devices during a commissioning session.
struct DiscoveredDevice {
    String mac;               // "AA:BB:CC:DD:EE:FF" form, used as the row key
    uint32_t xid;              // Transaction ID from the client's last request,
                                // echoed back so the client accepts our reply
    uint16_t flags;             // Client's BOOTP flags field (bit 0x8000 = "please
                                // broadcast the reply, my stack has no IP yet")
    unsigned long lastSeen;      // millis() timestamp, used to compute "age" for
                                // the dashboard's stale/fresh indicator
    String targetIp;             // IP the technician has assigned, "" = none yet
    uint8_t pendingReplyType;     // 0 = nothing to send; see sendNetworkReply()
                                  // for what 1/2/5 mean
    uint8_t clientMac[6];         // Raw 6-byte MAC, used to build the raw Ethernet
                                  // frame for unicast replies (see sendRawUnicastReply)
};

const int MAX_DEVICES = 20;
DiscoveredDevice devices[MAX_DEVICES];
int deviceCount = 0;

// --- Ping state ---
// These are written from the esp_ping library's internal FreeRTOS task
// (via the callbacks below) and read from the web server's request-handling
// context, hence `volatile`. There's no mutex because the /api/ping handler
// busy-waits for pingDone before reading any of the others, so there's no
// concurrent read/write window in practice.
volatile bool pingInProgress = false;
volatile bool pingDone = false;
volatile uint32_t pingTransmitted = 0;
volatile uint32_t pingReceived = 0;
volatile uint32_t pingTotalTimeMs = 0;
volatile uint32_t pingAccumulatedTimeMs = 0; // Running sum of round-trip times,
                                              // divided down to an average in onPingEnd()

// Called by the esp_ping library once per successful echo reply.
static void onPingSuccess(esp_ping_handle_t hdl, void *args) {
    uint32_t elapsed_time = 0;
    // ESP_PING_PROF_TIMEGAP = round-trip time of THIS packet, already in
    // whole milliseconds. Note: the underlying library truncates (not
    // rounds) sub-millisecond RTTs down to 0 -- on a direct local Ethernet
    // link this is common and does not mean the ping failed. The dashboard
    // JS floors the displayed value to 1ms in that case so it doesn't read
    // as "no time elapsed".
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time, sizeof(elapsed_time));
    pingAccumulatedTimeMs += elapsed_time;
}

// Called by the esp_ping library when a single echo request times out.
// Nothing to do here -- pingTransmitted/pingReceived (read in onPingEnd)
// already account for lost packets, so a per-timeout hook isn't needed.
static void onPingTimeout(esp_ping_handle_t hdl, void *args) {
    // Leave empty
}

// Called once after all pings in the session have completed (or timed out).
// This is where we compute the final summary the /api/ping handler returns.
static void onPingEnd(esp_ping_handle_t hdl, void *args) {
    uint32_t transmitted = 0, received = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));

    pingTransmitted = transmitted;
    pingReceived = received;

    // Calculate the average round-trip time in milliseconds
    if (received > 0) {
        pingTotalTimeMs = pingAccumulatedTimeMs / received;
    } else {
        pingTotalTimeMs = 0;
    }
    pingDone = true; // Wakes up the busy-wait loop in the /api/ping handler
}

// ==========================================
// HTML/JS DASHBOARD
// ==========================================
// The dashboard's HTML/CSS/JS (as a PROGMEM string called index_html) lives
// in webpage.h instead of directly in this file. It talks to the device
// purely through the small JSON API defined later in setup():
//   GET /api/devices       -> list of discovered devices
//   GET /api/settings      -> tool's own current IP/subnet
//   GET /api/set_network   -> save new IP/subnet, reboot
//   GET /api/assign        -> schedule a static IP for a MAC
//   GET /api/ping          -> ping an assigned device, wait for the result
//   GET /api/reboot        -> reboot on demand
//
// It's kept in its own header (rather than inline here) because Arduino's
// sketch preprocessor mis-scans C++11 raw string literals as ordinary code
// looking for function signatures to auto-prototype -- every JS
// "function name() {" in the dashboard's <script> matched that pattern and
// produced a bogus "function does not name a type" compile error. That
// scanner only runs on the .ino, not on included .h files, so moving the
// string there avoids the problem. See webpage.h for the actual markup.
#include "webpage.h"

// ==========================================
// HELPER FUNCTIONS
// ==========================================

// Parses a dotted-decimal string ("192.168.1.100") into an IPAddress.
// Silently returns 0.0.0.0 if the string doesn't parse -- callers that care
// about that should validate the input themselves before calling this.
IPAddress stringToIP(String ipStr) {
    IPAddress ip;
    ip.fromString(ipStr);
    return ip;
}

// Formats a raw 6-byte MAC into the familiar "AA:BB:CC:DD:EE:FF" string used
// as the device table's row key throughout the dashboard.
String macToString(const uint8_t* mac) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

// Computes the subnet-directed broadcast address for a given IP/mask pair,
// e.g. 192.168.1.100 / 255.255.255.0 -> 192.168.1.255. Used when a client's
// flags say it can't yet receive a unicast reply (see clientWantsBroadcast
// in sendNetworkReply()).
IPAddress calcBroadcast(IPAddress ip, IPAddress subnet) {
    IPAddress bcast;
    for (int i = 0; i < 4; i++) {
        bcast[i] = ip[i] | (~subnet[i] & 0xFF);
    }
    return bcast;
}

// Standard 16-bit one's-complement Internet checksum (RFC 791), used for the
// IPv4 header checksum in sendRawUnicastReply(). Treats `data` as a stream
// of big-endian 16-bit words; handles an odd trailing byte by padding with
// zero.
uint16_t ipChecksum(const uint8_t* data, int len) {
    uint32_t sum = 0;
    for (int i = 0; i < len; i += 2) {
        uint16_t word = data[i] << 8;
        if (i + 1 < len) word |= data[i + 1];
        sum += word;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

// Hand-builds and transmits a raw Ethernet frame (Ethernet + IPv4 + UDP
// headers, no ARP lookup) addressed directly to the target device's MAC.
// This is used instead of a normal UDP send because the device we're
// replying to typically doesn't have a working IP yet (that's the whole
// point of this tool) or the client didn't ask us to reply via broadcast --
// going straight to raw Ethernet sidesteps needing ARP to resolve an IP
// the device may not even be configured with yet.
void sendRawUnicastReply(uint8_t* payload, uint16_t payloadLen, IPAddress dstIP, uint8_t* dstMac, IPAddress srcIP) {
    uint8_t frame[14 + 20 + 8 + 300]; // Ethernet(14) + IPv4(20) + UDP(8) + payload
    uint8_t ownMac[6];
    ETH.macAddress(ownMac);

    int idx = 0;
    // --- Ethernet header ---
    memcpy(&frame[idx], dstMac, 6);
    idx += 6;
    memcpy(&frame[idx], ownMac, 6); idx += 6;
    frame[idx++] = 0x08; frame[idx++] = 0x00; // EtherType: IPv4

    // --- IP header ---
    int ipStart = idx;
    uint16_t totalLen = 20 + 8 + payloadLen;
    frame[idx++] = 0x45; // Version 4, IHL 5 (20-byte header, no options)
    frame[idx++] = 0x00; // DSCP/ECN
    frame[idx++] = (totalLen >> 8) & 0xFF;
    frame[idx++] = totalLen & 0xFF;
    frame[idx++] = 0x00; frame[idx++] = 0x00; // Identification (fine to leave 0, no fragmentation)
    frame[idx++] = 0x00; frame[idx++] = 0x00; // Flags/Fragment offset
    frame[idx++] = 0x40; // TTL 64
    frame[idx++] = 0x11; // Protocol: UDP (17)
    frame[idx++] = 0x00; frame[idx++] = 0x00; // Header checksum -- computed below, filled in after
    frame[idx++] = srcIP[0]; frame[idx++] = srcIP[1]; frame[idx++] = srcIP[2]; frame[idx++] = srcIP[3];
    frame[idx++] = dstIP[0]; frame[idx++] = dstIP[1]; frame[idx++] = dstIP[2]; frame[idx++] = dstIP[3];

    // Now that the full 20-byte IP header is written, checksum it and patch
    // the two checksum bytes we left as 0x00 above.
    uint16_t csum = ipChecksum(&frame[ipStart], 20);
    frame[ipStart + 10] = (csum >> 8) & 0xFF;
    frame[ipStart + 11] = csum & 0xFF;

    // --- UDP header ---
    frame[idx++] = (BOOTP_SERVER_PORT >> 8) & 0xFF; frame[idx++] = BOOTP_SERVER_PORT & 0xFF; // src port 67
    frame[idx++] = (BOOTP_CLIENT_PORT >> 8) & 0xFF; frame[idx++] = BOOTP_CLIENT_PORT & 0xFF; // dst port 68
    uint16_t udpLen = 8 + payloadLen;
    frame[idx++] = (udpLen >> 8) & 0xFF; frame[idx++] = udpLen & 0xFF;
    frame[idx++] = 0x00; frame[idx++] = 0x00; // UDP checksum 0 = "not computed" (legal for IPv4)

    memcpy(&frame[idx], payload, payloadLen);
    idx += payloadLen;

    esp_eth_handle_t eth_handle = ETH.handle();
    if (eth_handle) {
        esp_eth_transmit(eth_handle, frame, idx);
    }
}

// ==========================================
// UNIVERSAL BOOTP / DHCP LOGIC
// ==========================================
// Reference: fixed-length layout of a BOOTP/DHCP packet (RFC 951 / RFC 2131).
// All the "magic number" byte offsets below (16, 28, 236...) come straight
// from this layout -- keep it handy when debugging a malformed reply.
//
//   offset  size  field
//   0        1    op        (1 = BOOTREQUEST, 2 = BOOTREPLY)
//   1        1    htype     (1 = Ethernet)
//   2        1    hlen      (6 = MAC address length)
//   3        1    hops
//   4        4    xid       (transaction ID, echoed back from the request)
//   8        2    secs
//   10       2    flags     (bit 0x8000 = client wants a broadcast reply)
//   12       4    ciaddr    (client's current IP, usually 0 before it has one)
//   16       4    yiaddr    ("your" IP -- this is the IP WE are assigning)
//   20       4    siaddr    (next-server IP, unused here)
//   24       4    giaddr    (relay agent IP, unused here)
//   28       16   chaddr    (client hardware address; only first 6 bytes used)
//   44       64   sname     (optional server host name, unused here)
//   108      128  file      (optional boot file name, unused here)
//   236      4    magic cookie (0x63 0x82 0x53 0x63, marks the start of options)
//   240      var  options   (TLV-encoded DHCP options, terminated by 0xFF)

// Builds and sends the reply for one device that's waiting on one
// (pendingReplyType != 0, checked by the caller in loop()).
void sendNetworkReply(DiscoveredDevice &dev) {
    uint8_t reply[300] = {0}; // Zero-filled so every unused field defaults to 0
    reply[0] = 0x02; // Reply
    reply[1] = 0x01; // Ethernet
    reply[2] = 0x06; // MAC Length
    reply[3] = 0x00; // Hops

    // Echo back the client's transaction ID so it recognizes this as the
    // answer to its specific request.
    reply[4] = (dev.xid >> 24) & 0xFF;
    reply[5] = (dev.xid >> 16) & 0xFF;
    reply[6] = (dev.xid >> 8) & 0xFF;
    reply[7] = dev.xid & 0xFF;
    // Mirror the client's exact flags (including the broadcast bit)
    reply[10] = (dev.flags >> 8) & 0xFF;
    reply[11] = dev.flags & 0xFF;

    // yiaddr = the static IP the technician assigned to this device
    IPAddress targetIP = stringToIP(dev.targetIp);
    reply[16] = targetIP[0]; reply[17] = targetIP[1];
    reply[18] = targetIP[2]; reply[19] = targetIP[3];

    // siaddr left at 0.0.0.0 -- we're not pointing the client at a separate
    // boot/config server, everything it needs is in the options below.
    reply[20] = 0; reply[21] = 0; reply[22] = 0; reply[23] = 0;

    // chaddr = the requesting device's own MAC, so it recognizes the reply
    // is addressed to it.
    memcpy(&reply[28], dev.clientMac, 6);
    IPAddress myIP = stringToIP(current_eth_ip);
    IPAddress subnet = stringToIP(current_eth_sn);

    int idx = 236;
    reply[idx++] = 0x63; // Magic Cookie (marks start of DHCP options)
    reply[idx++] = 0x82;
    reply[idx++] = 0x53;
    reply[idx++] = 0x63;

    // pendingReplyType doubles as BOTH "what kind of reply to send" and,
    // when it's 2 or 5, the literal DHCP option-53 message-type byte --
    // that's not a coincidence: DHCPOFFER really is type 2 and DHCPACK
    // really is type 5 in the spec, so the same value works for both. A
    // pendingReplyType of 1 means "plain BOOTP reply", which is NOT a real
    // DHCP type and deliberately skips the DHCP-only options block below
    // (a bare BOOTP client wouldn't understand them anyway).
    if (dev.pendingReplyType == 2 || dev.pendingReplyType == 5) {
        reply[idx++] = 53; // Option 53: DHCP Message Type
        reply[idx++] = 1;  // length 1
        reply[idx++] = dev.pendingReplyType; // 2 = OFFER, 5 = ACK

        reply[idx++] = 54; // Option 54: DHCP Server Identifier
        reply[idx++] = 4;  // length 4
        reply[idx++] = myIP[0]; reply[idx++] = myIP[1];
        reply[idx++] = myIP[2]; reply[idx++] = myIP[3];

        reply[idx++] = 51; // Option 51: IP Address Lease Time
        reply[idx++] = 4;  // length 4
        reply[idx++] = 0x00; reply[idx++] = 0x00; // 0x00000E10 = 3600 seconds (1 hour)
        reply[idx++] = 0x0E; reply[idx++] = 0x10;
    }

    reply[idx++] = 1; // Option 1: Subnet Mask
    reply[idx++] = 4;
    reply[idx++] = subnet[0]; reply[idx++] = subnet[1];
    reply[idx++] = subnet[2]; reply[idx++] = subnet[3];

    reply[idx++] = 3; // Option 3: Router (gateway) -- we advertise ourselves
    reply[idx++] = 4;
    reply[idx++] = myIP[0]; reply[idx++] = myIP[1];
    reply[idx++] = myIP[2]; reply[idx++] = myIP[3];

    reply[idx++] = 255; // Option 255: End of options

    // If the client's flags say it can't accept a unicast reply yet (no IP
    // configured), fall back to a subnet broadcast; otherwise go straight
    // to its MAC with a raw frame (see sendRawUnicastReply for why raw).
    bool clientWantsBroadcast = (dev.flags & 0x8000) != 0;

    if (clientWantsBroadcast) {
        IPAddress bcastAddr = calcBroadcast(myIP, subnet);
        udp.writeTo(reply, 300, bcastAddr, BOOTP_CLIENT_PORT);
    } else {
        sendRawUnicastReply(reply, 300, targetIP, dev.clientMac, myIP);
    }

    String typeStr = (dev.pendingReplyType == 1) ?
    "BOOTP Reply" : (dev.pendingReplyType == 2 ? "DHCP Offer" : "DHCP Ack");
    Serial.printf("Sent %s to %s with IP %s\n", typeStr.c_str(), dev.mac.c_str(), dev.targetIp.c_str());
}

// UDP packet handler registered on port 67 in setup(). Runs every time a
// BOOTP/DHCP request arrives on the wired Ethernet interface. Parses just
// enough of the packet to identify the device and what it's asking for,
// then either updates its existing table entry or adds a new one.
void parseIncomingPacket(AsyncUDPPacket &packet) {
    if (packet.length() < 236) return; // Shorter than the fixed BOOTP header -- not a real request

    uint8_t* data = packet.data();
    if (data[0] != 0x01) return; // op != BOOTREQUEST, ignore (e.g. we're seeing our own reply echoed)

    uint32_t xid = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
    uint16_t clientFlags = (data[10] << 8) | data[11];

    uint8_t macBuf[6];
    memcpy(macBuf, &data[28], 6); // chaddr
    String macStr = macToString(macBuf);

    // DHCP requests carry a message-type option (53) inside the options
    // block; plain BOOTP requests have no options at all, so
    // incomingDhcpType stays 0 for those.
    uint8_t incomingDhcpType = 0;
    if (data[236] == 0x63 && data[237] == 0x82 && data[238] == 0x53 && data[239] == 0x63) {
        int idx = 240;
        // Walk the TLV-encoded options looking specifically for option 53;
        // everything else is skipped over using its own length byte.
        while (idx < packet.length() && data[idx] != 255) {
            uint8_t opt = data[idx++];
            if (opt == 0) continue; // Pad byte, has no length field
            uint8_t len = data[idx++];
            if (opt == 53 && len == 1) {
                incomingDhcpType = data[idx];
                break;
            }
            idx += len;
        }
    }

    String reqType = "BOOTP";
    if (incomingDhcpType == 1) reqType = "DHCP Discover";
    else if (incomingDhcpType == 3) reqType = "DHCP Request";

    Serial.printf("Received %s from: %s\n", reqType.c_str(), macStr.c_str());

    bool found = false;
    for(int i = 0; i < deviceCount; i++) {
        if (devices[i].mac == macStr) {
            // Known device -- refresh its "last seen" timestamp/xid/flags
            // regardless of whether it has an assigned IP yet, so the
            // dashboard's freshness indicator stays accurate.
            devices[i].lastSeen = millis();
            devices[i].xid = xid;
            devices[i].flags = clientFlags;
            memcpy(devices[i].clientMac, macBuf, 6);
            found = true;
            if (devices[i].targetIp != "") {
                // Map the request type to the matching reply type. A plain
                // BOOTP request (incomingDhcpType 0) always gets a plain
                // BOOTP reply; DHCP DISCOVER (1) gets OFFER (2); DHCP
                // REQUEST (3) gets ACK (5). See the pendingReplyType note
                // in sendNetworkReply() for why these numbers were chosen.
                if (incomingDhcpType == 0) {
                    devices[i].pendingReplyType = 1;
                } else if (incomingDhcpType == 1) {
                    devices[i].pendingReplyType = 2;
                } else if (incomingDhcpType == 3) {
                    devices[i].pendingReplyType = 5;
                }
            }
            break;
        }
    }

    if (!found && deviceCount < MAX_DEVICES) {
        // New device -- add it to the table with no assigned IP yet. It'll
        // just sit there (pendingReplyType 0, i.e. "no reply queued") until
        // a technician assigns it one via the dashboard.
        devices[deviceCount].mac = macStr;
        devices[deviceCount].lastSeen = millis();
        devices[deviceCount].targetIp = "";
        devices[deviceCount].xid = xid;
        devices[deviceCount].flags = clientFlags;
        devices[deviceCount].pendingReplyType = 0;
        memcpy(devices[deviceCount].clientMac, macBuf, 6);
        deviceCount++;
        // Note: if the table is already full (MAX_DEVICES), new devices are
        // silently dropped here. Bump MAX_DEVICES if a site regularly has
        // more than 20 devices being commissioned at once.
    }
}

// ==========================================
// SETUP & LOOP
// ==========================================
void setup() {
    Serial.begin(38400);
    delay(1000);

    // Load the tool's own Ethernet IP/subnet from flash, falling back to
    // the compiled-in defaults on first boot.
    prefs.begin("network", false);
    current_eth_ip = prefs.getString("eth_ip", "192.168.1.100");
    current_eth_sn = prefs.getString("eth_sn", "255.255.255.0");

    IPAddress eth_ip = stringToIP(current_eth_ip);
    IPAddress eth_sn = stringToIP(current_eth_sn);
    IPAddress eth_gw = eth_ip; // The tool advertises itself as the gateway (see Option 3 above)

    // --- Wi-Fi: connects to the technician's hotspot to host the dashboard ---
    WiFi.mode(WIFI_STA);
    WiFi.begin(HOTSPOT_SSID, HOTSPOT_PASS);
    Serial.print("Connecting to Mobile Hotspot");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nConnected to Hotspot!");
    Serial.print("Web UI is now at: http://");
    Serial.println(WiFi.localIP());

    // With both Wi-Fi and Ethernet active, ESP-IDF needs to be told which
    // interface is the "default route" for traffic that doesn't specify
    // one explicitly (like the ICMP ping in /api/ping). Once the Ethernet
    // link comes up, make it the default so field-network traffic doesn't
    // accidentally try to go out over Wi-Fi.
    WiFi.onEvent([](arduino_event_id_t event) {
        if (event == ARDUINO_EVENT_ETH_CONNECTED || event == ARDUINO_EVENT_ETH_GOT_IP) {
            esp_netif_t* eth_netif = ETH.netif();
            if (eth_netif) {
                esp_netif_set_default_netif(eth_netif);
                Serial.println("Ethernet link up: Ethernet is now the default route.");
            }
        }
    });

    // --- Ethernet (W5500 over SPI): the field network BOOTP/DHCP is served on ---
    spi.begin(ETH_SPI_SCK, ETH_SPI_MISO, ETH_SPI_MOSI, ETH_PHY_CS);
    ETH.config(eth_ip, eth_gw, eth_sn);
    if(!ETH.begin(ETH_PHY_TYPE, ETH_PHY_ADDR, ETH_PHY_CS, ETH_PHY_IRQ, ETH_PHY_RST, spi)) {
        Serial.println("Failed to initialize W5500 module.");
    }

    // Bind the BOOTP/DHCP listener to port 67. Every inbound packet here
    // gets handed to parseIncomingPacket().
    if(udp.listen(BOOTP_SERVER_PORT)) {
        Serial.println("DHCP/BOOTP Server listening on Port 67 (Ethernet)");
        udp.onPacket([](AsyncUDPPacket packet) {
            parseIncomingPacket(packet);
        });
    } else {
        Serial.println("CRITICAL ERROR: Failed to bind UDP Port 67!");
    }

    // --- Web dashboard + JSON API routes ---
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send_P(200, "text/html", index_html);
    });

    // Reports the tool's own current IP/subnet for the "Tool Network
    // Settings" card.
    server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest *request){
        String json = "{\"ip\":\"" + current_eth_ip + "\",\"sn\":\"" + current_eth_sn + "\"}";
        request->send(200, "application/json", json);
    });

    // Saves a new IP/subnet for the TOOL ITSELF (not a device) and triggers
    // a delayed reboot so the new Ethernet config takes effect.
    server.on("/api/set_network", HTTP_GET, [](AsyncWebServerRequest *request){
        if(request->hasParam("ip") && request->hasParam("sn")){
            prefs.putString("eth_ip", request->getParam("ip")->value());
            prefs.putString("eth_sn", request->getParam("sn")->value());
            request->send(200, "text/plain", "Settings saved. Rebooting...");
            shouldReboot = true;
            rebootTimer = millis();
        } else {
             request->send(400, "text/plain", "Missing parameters");
        }
    });

    // Returns the full discovered-device table as JSON for the dashboard's
    // polling loop (loadData() in the embedded JS).
    server.on("/api/devices", HTTP_GET, [](AsyncWebServerRequest *request){
        String json = "[";
        unsigned long now = millis();
        for(int i = 0; i < deviceCount; i++) {
            if(i > 0) json += ",";
            json += "{\"mac\":\"" + devices[i].mac + "\",\"age\":" + String((now - devices[i].lastSeen)/1000) + ",\"targetIp\":\"" + devices[i].targetIp + "\"}";
        }
        json += "]";
        request->send(200, "application/json", json);
    });

    // Records the technician's chosen static IP for a MAC. Doesn't send
    // anything itself -- the actual reply goes out the next time that
    // device sends a BOOTP/DHCP request (see parseIncomingPacket + loop()).
    server.on("/api/assign", HTTP_GET, [](AsyncWebServerRequest *request){
        if(request->hasParam("mac") && request->hasParam("ip")){
            String mac = request->getParam("mac")->value();
            String ip = request->getParam("ip")->value();
            for(int i = 0; i < deviceCount; i++){
                if(devices[i].mac == mac) {
                    devices[i].targetIp = ip;
                    request->send(200, "text/plain", "IP Scheduled! Sent on next broadcast.");
                    return;
                }
            }
            request->send(404, "text/plain", "Device not found");
        } else {
            request->send(400, "text/plain", "Missing params");
        }
    });

    server.on("/api/reboot", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/plain", "Rebooting...");
        shouldReboot = true;
        rebootTimer = millis();
    });

    // Pings a device's assigned IP over the wired Ethernet interface so a
    // technician can confirm the static IP actually took effect. See the
    // comments on onPingSuccess/onPingEnd above for how the timing is
    // measured.
    server.on("/api/ping", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!request->hasParam("ip")) {
            request->send(400, "application/json", "{\"error\":\"Missing ip parameter\"}");
            return;
        }
        if (pingInProgress) {
            request->send(409, "application/json", "{\"error\":\"A ping is already running\"}");
            return;
        }

        IPAddress target = stringToIP(request->getParam("ip")->value());
        ip_addr_t targetAddr;
        IP_ADDR4(&targetAddr, target[0], target[1], target[2], target[3]);

        esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
        config.target_addr = targetAddr;
        config.count = 4;
        config.interval_ms = 250;
        config.timeout_ms = 750;

        // Force ping out the wired Ethernet port
        // Note: esp_ping's SO_BINDTODEVICE step is skipped entirely if this
        // index happens to be 0 (0 reads as "unset" internally), which
        // would let the ping fall back to the default route instead of
        // being pinned to Ethernet. Worth checking esp_netif_get_netif_impl_index()'s
        // return value here if pings ever seem to go out the wrong interface.
        esp_netif_t* eth_netif = ETH.netif();
        if (eth_netif) {
            config.interface = esp_netif_get_netif_impl_index(eth_netif);
        }

        esp_ping_callbacks_t cbs = {};
        cbs.on_ping_success = onPingSuccess;
        cbs.on_ping_timeout = onPingTimeout;
        cbs.on_ping_end = onPingEnd;
        cbs.cb_args = NULL;

        pingInProgress = true;
        pingDone = false;
        pingTransmitted = 0;
        pingReceived = 0;
        pingTotalTimeMs = 0;
        pingAccumulatedTimeMs = 0; // Reset the accumulator

        esp_ping_handle_t pingHandle;
        if (esp_ping_new_session(&config, &cbs, &pingHandle) != ESP_OK) {
            pingInProgress = false;
            request->send(500, "application/json", "{\"error\":\"Failed to start ping session\"}");
            return;
        }
        esp_ping_start(pingHandle);

        // Bounded wait so a bad target can't hang the web server indefinitely.
        // The actual pinging happens in esp_ping's own FreeRTOS task; this
        // loop just blocks the HTTP handler until that task reports pingDone
        // (via onPingEnd) or the 4-second ceiling is hit.
        unsigned long start = millis();
        while (!pingDone && (millis() - start < 4000)) {
            delay(50);
        }

        esp_ping_stop(pingHandle);
        esp_ping_delete_session(pingHandle);

        pingInProgress = false;
        String json = "{\"transmitted\":" + String(pingTransmitted) +
                      ",\"received\":" + String(pingReceived) +
                      ",\"timeMs\":" + String(pingTotalTimeMs) + "}";
        request->send(200, "application/json", json);
    });

    server.begin();
}

void loop() {
    // Delayed reboot: gives the HTTP response for /api/set_network or
    // /api/reboot time to actually reach the browser before we restart.
    if (shouldReboot && (millis() - rebootTimer > 2000)) {
        ESP.restart();
    }

    // Flush any queued BOOTP/DHCP replies. A device gets pendingReplyType
    // set (in parseIncomingPacket) when it has an assigned IP and just sent
    // a new request; this is where that reply actually goes out.
    for (int i = 0; i < deviceCount; i++) {
        if (devices[i].pendingReplyType != 0) {
            delay(15); // Small gap before replying; gives the client's stack
                       // a moment to finish processing its own request before
                       // it has to handle our reply.
            sendNetworkReply(devices[i]);
            devices[i].pendingReplyType = 0;
        }
    }

    delay(10);
}
