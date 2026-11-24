#include "EthernetLink.h"

#if defined(WITH_ETHERNET_UPLINK)

#include <ESP32_CH390.h>
#include <WiFi.h>
#include <atomic>
#include <stdio.h>
#include <string.h>

#include "UplinkPolicy.h"

// Arduino-ESP32's WiFi layer (WiFi.hostByName(), status bits, event dispatch) relies on an event
// group that is only created by tcpipInit(). Arduino's own ETH.begin() and WiFi init call it,
// but the CH390 library only calls esp_netif_init(). Without this, Ethernet-only operation
// (Wi-Fi never started) crashes in the DNS callback: "xEventGroupSetBits ... (xEventGroup)".
// Idempotent; ETH.cpp declares it the same way (name-mangling ignores the return type).
extern bool tcpipInit();

namespace {

// Written from the esp_event task, read from the main loop.
std::atomic<bool> s_link_flag(false);
bool s_hw_ok = false;
UplinkPolicy s_policy;

void onEthEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_CONNECTED:
      s_link_flag = true;
      Serial.println("[ETH] link up");
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
    case ARDUINO_EVENT_ETH_STOP:
      s_link_flag = false;
      Serial.println("[ETH] link down");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.printf("[ETH] got ip %s\n", CH390.localIP().toString().c_str());
      break;
    default:
      break;
  }
}

uint32_t raw(const IPAddress& ip) {
  return static_cast<uint32_t>(ip);
}

// Copies the dotted-quad into `out`, or "--" when the address is unset.
void ipText(const IPAddress& ip, char* out, size_t out_size) {
  if (raw(ip) == 0) {
    snprintf(out, out_size, "--");
  } else {
    snprintf(out, out_size, "%s", ip.toString().c_str());
  }
}

const char* duplexText(bool up) {
  if (!up) return "--";
  return CH390.fullDuplex() ? "full" : "half";
}

}  // namespace

bool EthernetLink::begin() {
  if (s_hw_ok) {
    return true;
  }
  if (!tcpipInit()) {
    Serial.println("[ETH] tcpip init failed; Ethernet disabled");
    return false;
  }
  // Register first so a fast link-up event is not missed.
  WiFi.onEvent(onEthEvent);

  // Pins only; the SPI host/clock are left at the library defaults, exactly as the
  // upstream ThinkNode M7 companion Ethernet build does (the radio uses its own SPIClass).
  ch390_config_t config = CH390_DEFAULT_CONFIG();
  config.spi_miso_gpio = ETH_MISO_PIN;
  config.spi_mosi_gpio = ETH_MOSI_PIN;
  config.spi_sck_gpio = ETH_SCLK_PIN;
  config.spi_cs_gpio = ETH_CS_PIN;
  config.int_gpio = ETH_INT_PIN;
  if (!CH390.begin(config)) {
    Serial.println("[ETH] CH390 init failed; Ethernet disabled");
    return false;
  }
  s_hw_ok = true;
  s_policy.begin(millis());
  Serial.println("[ETH] CH390 started, waiting for link");
  return true;
}

void EthernetLink::update(uint32_t now_ms) {
  if (!s_hw_ok) {
    return;
  }
  const char* before = UplinkPolicy::stateName(s_policy.state());
  s_policy.update(now_ms, linkUp(), hasIP());
  const char* after = UplinkPolicy::stateName(s_policy.state());
  if (strcmp(before, after) != 0) {  // the LINK_LOST debounce shares the "no_link" name
    Serial.printf("[ETH] state %s -> %s\n", before, after);
  }
}

bool EthernetLink::isEnabled() {
  return s_hw_ok;
}

bool EthernetLink::holdsWifiOff() {
  return s_hw_ok && s_policy.holdsWifiOff();
}

// The library's own linkUp()/isConnected() only report "has an address", and ETH_LOST_IP is
// not forwarded as an event, so the address is polled; carrier comes from events.
bool EthernetLink::hasIP() {
  return s_hw_ok && raw(CH390.localIP()) != 0;
}

bool EthernetLink::linkUp() {
  return s_hw_ok && (s_link_flag.load() || hasIP());  // tolerate a missed link event
}

IPAddress EthernetLink::localIP() {
  return s_hw_ok ? CH390.localIP() : IPAddress(0, 0, 0, 0);
}

IPAddress EthernetLink::uplinkIP() {
  return hasIP() ? CH390.localIP() : WiFi.localIP();
}

const char* EthernetLink::stateName() {
  return UplinkPolicy::stateName(s_policy.state());
}

void EthernetLink::formatStatus(char* reply, size_t reply_size, const char* uplink) {
  if (!s_hw_ok) {
    snprintf(reply, reply_size, "> eth:unavailable");
    return;
  }
  const bool up = linkUp();
  char ip[16], gw[16], dns[16];
  ipText(CH390.localIP(), ip, sizeof(ip));
  ipText(CH390.gatewayIP(), gw, sizeof(gw));
  ipText(CH390.dnsIP(0), dns, sizeof(dns));
  const String mac = CH390.macAddress();
  snprintf(reply, reply_size, "> state:%s link:%s ip:%s gw:%s dns:%s mac:%s speed:%dM duplex:%s uplink:%s", stateName(),
           up ? "up" : "down", ip, gw, dns, mac.c_str(), up ? CH390.linkSpeed() : 0, duplexText(up), uplink);
}

size_t EthernetLink::formatJson(char* out, size_t out_size, const char* uplink, bool wifi_held_off) {
  if (!s_hw_ok) {
    int n = snprintf(out, out_size, "{\"enabled\":false}");
    return (n > 0 && static_cast<size_t>(n) < out_size) ? static_cast<size_t>(n) : 0;
  }
  const bool up = linkUp();
  const bool has_ip = hasIP();
  char ip[16], mask[16], gw[16], dns[16];
  ipText(CH390.localIP(), ip, sizeof(ip));
  ipText(CH390.subnetMask(), mask, sizeof(mask));
  ipText(CH390.gatewayIP(), gw, sizeof(gw));
  ipText(CH390.dnsIP(0), dns, sizeof(dns));
  const String mac = CH390.macAddress();
  int n = snprintf(out, out_size,
                   "{\"enabled\":true,\"state\":\"%s\",\"link\":%s,\"connected\":%s,\"ip\":\"%s\",\"mask\":\"%s\","
                   "\"gateway\":\"%s\",\"dns\":\"%s\",\"mac\":\"%s\",\"speed\":%d,\"duplex\":\"%s\","
                   "\"uplink\":\"%s\",\"wifi_held_off\":%s}",
                   stateName(), up ? "true" : "false", has_ip ? "true" : "false", ip, mask, gw, dns, mac.c_str(),
                   up ? CH390.linkSpeed() : 0, duplexText(up), uplink, wifi_held_off ? "true" : "false");
  return (n > 0 && static_cast<size_t>(n) < out_size) ? static_cast<size_t>(n) : 0;
}

#endif  // WITH_ETHERNET_UPLINK
