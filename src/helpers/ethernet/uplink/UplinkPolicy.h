#pragma once

#include <stdint.h>

// Decides whether Ethernet currently "owns" the uplink, i.e. whether Wi-Fi must be held off.
//
// This is deliberately pure logic (no Arduino/ESP-IDF dependencies): it is fed the polled
// link/IP state plus a millisecond clock, so it can be unit tested on the host.
//
// Behaviour:
//   - While Ethernet is detecting, connecting or connected, Wi-Fi is held off.
//   - Once the cable is gone (or Ethernet never got an address), Wi-Fi is released so the
//     node falls back to it. If Ethernet later recovers, the hold applies again.
class UplinkPolicy {
public:
  enum State : uint8_t {
    UPLINK_DISABLED,    // no Ethernet hardware / init failed: Wi-Fi is never held off
    UPLINK_DETECTING,   // just started, waiting for the PHY to report a link (Wi-Fi held off)
    UPLINK_NO_LINK,     // no carrier (Wi-Fi allowed)
    UPLINK_CONNECTING,  // link up, waiting for an address (Wi-Fi held off)
    UPLINK_CONNECTED,   // link up with an address (Wi-Fi held off)
    UPLINK_NO_IP,       // link up but no address for too long (Wi-Fi allowed as fallback)
    UPLINK_LINK_LOST,   // carrier just dropped; short hold-off to ride out flaps (Wi-Fi held off)
  };

  // How long to wait for the PHY to auto-negotiate after start before assuming no cable.
  static uint32_t detectMillis() { return 5000UL; }
  // How long a link may sit without an address before Wi-Fi is allowed as a fallback.
  static uint32_t ipWaitMillis() { return 60000UL; }
  // How long to keep Wi-Fi off after the carrier drops, to avoid thrash on brief flaps.
  static uint32_t lossHoldMillis() { return 3000UL; }

  UplinkPolicy() : _state(UPLINK_DISABLED), _since(0) {}

  // Call once the Ethernet hardware initialised successfully.
  void begin(uint32_t now_ms) {
    _state = UPLINK_DETECTING;
    _since = now_ms;
  }

  // Call every loop tick with the current polled state.
  void update(uint32_t now_ms, bool link_up, bool has_ip) {
    if (_state == UPLINK_DISABLED) {
      return;
    }
    if (has_ip) {
      enter(UPLINK_CONNECTED, now_ms);
      return;
    }
    if (link_up) {
      if (_state != UPLINK_CONNECTING && _state != UPLINK_NO_IP) {
        enter(UPLINK_CONNECTING, now_ms);  // link just came up, or the address was lost
      } else if (_state == UPLINK_CONNECTING && elapsed(now_ms) >= ipWaitMillis()) {
        enter(UPLINK_NO_IP, now_ms);
      }
      return;
    }
    // No carrier.
    switch (_state) {
      case UPLINK_DETECTING:
        if (elapsed(now_ms) >= detectMillis()) {
          enter(UPLINK_NO_LINK, now_ms);
        }
        break;
      case UPLINK_CONNECTING:
      case UPLINK_CONNECTED:
      case UPLINK_NO_IP:
        enter(UPLINK_LINK_LOST, now_ms);
        break;
      case UPLINK_LINK_LOST:
        if (elapsed(now_ms) >= lossHoldMillis()) {
          enter(UPLINK_NO_LINK, now_ms);
        }
        break;
      default:
        break;
    }
  }

  State state() const { return _state; }
  bool isEnabled() const { return _state != UPLINK_DISABLED; }

  // True while Wi-Fi must stay off because Ethernet is (or is about to be) the uplink.
  bool holdsWifiOff() const {
    return _state == UPLINK_DETECTING || _state == UPLINK_CONNECTING || _state == UPLINK_CONNECTED || _state == UPLINK_LINK_LOST;
  }

  static const char* stateName(State s) {
    switch (s) {
      case UPLINK_DETECTING:  return "detecting";
      case UPLINK_NO_LINK:    return "no_link";
      case UPLINK_CONNECTING: return "connecting";
      case UPLINK_CONNECTED:  return "connected";
      case UPLINK_NO_IP:      return "no_ip";
      case UPLINK_LINK_LOST:  return "no_link";  // internal debounce; the cable is gone either way
      default:         return "unavailable";
    }
  }

private:
  uint32_t elapsed(uint32_t now_ms) const { return static_cast<uint32_t>(now_ms - _since); }
  void enter(State s, uint32_t now_ms) {
    if (_state != s) {
      _state = s;
      _since = now_ms;
    }
  }

  State _state;
  uint32_t _since;
};
