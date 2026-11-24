#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

// Ethernet (CH390 over SPI) as a network uplink for observer builds, alongside Wi-Fi.
// Only compiled when WITH_ETHERNET_UPLINK is defined (ThinkNode M7 observer).
//
// NetworkService polls update() each loop and asks holdsWifiOff(): Wi-Fi stays off while
// Ethernet is detecting/connecting/connected and is released again (fallback) when it is lost.
class EthernetLink {
public:
  // Initialise the CH390. Returns false if the hardware did not come up, in which
  // case Ethernet stays disabled and Wi-Fi is never held off.
  static bool begin();

  // Poll link/IP state and advance the arbitration policy. Call every loop tick.
  static void update(uint32_t now_ms);

  static bool isEnabled();
  static bool holdsWifiOff();
  static bool hasIP();
  static bool linkUp();
  static IPAddress localIP();
  // Address clients should use to reach this node: Ethernet's when it has one, else the Wi-Fi STA address.
  static IPAddress uplinkIP();
  static const char* stateName();

  // "> state:... " style reply for `get eth.status` (kept under 160 bytes).
  static void formatStatus(char* reply, size_t reply_size, const char* uplink);
  // JSON object (including braces) for the web panel stats. Returns bytes written, 0 on overflow.
  static size_t formatJson(char* out, size_t out_size, const char* uplink, bool wifi_held_off);
};
