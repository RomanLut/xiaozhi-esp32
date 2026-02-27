#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_bt_defs.h"
#include "esp_err.h"

class ILink {
public:
    // Describes a lamp found during BLE scanning.
    struct LampInfo {
        esp_bd_addr_t bda; // BLE MAC address of the discovered lamp
        int8_t rssi;       // Received signal strength in dBm (typically about -100..-30; less negative = stronger)
        bool has_a032;     // True if the lamp advertised the iLink service UUID 0xA032
        char name[32];     // Advertised BLE local name (empty string if not present)
    };

    // Initializes the shared BLE backend used by discovery and control.
    static esp_err_t init();
    // Scans for nearby iLink lamps and writes up to max_lamps results into out_lamps.
    static size_t getLamps(LampInfo *out_lamps, size_t max_lamps, uint32_t scan_ms = 3000);

    // Creates a controller bound to a specific lamp BLE MAC address.
    explicit ILink(const esp_bd_addr_t bda);

    // Returns true if bda matches this controller's target lamp MAC.
    bool matchesTargetBda(const esp_bd_addr_t bda) const;
    // Returns the target lamp MAC address used by this controller.
    const uint8_t *targetBda() const;

    // Connects to the target lamp and discovers the iLink command characteristic.
    esp_err_t connect(uint32_t timeout_ms = 8000);
    // Returns true when connected and ready to send commands to the target lamp.
    bool isReady() const;
    // Disconnects from the target lamp (BLE disconnect is asynchronous).
    esp_err_t disconnect();

    // Turns the lamp on.
    esp_err_t turnOn();
    // Turns the lamp off.
    esp_err_t turnOff();
    // Sets white mode using 0..100 brightness and 0..100 temperature (cold..warm).
    esp_err_t setWhite(uint8_t brightness_percent, uint8_t temperature_percent);
    // Sets brightness only (0..100). Temperature is unchanged.
    esp_err_t setBrightness(uint8_t brightness_percent);
    // Sets color temperature only (0..100, cold..warm). Brightness is unchanged.
    esp_err_t setTemperature(uint8_t temperature_percent);
    // Sets RGB color using per-channel percentages in range 0..100.
    esp_err_t setRGB(uint8_t r_percent, uint8_t g_percent, uint8_t b_percent);

private:
    esp_bd_addr_t target_bda_; // Target lamp BLE MAC address.

    // Sends a raw iLink command over the connected A040 characteristic.
    esp_err_t write(const uint8_t *data, uint16_t len) const;
    // Builds/sends a 1-parameter iLink command frame.
    esp_err_t sendStdCmd(uint8_t cmd_hi, uint8_t cmd_lo, uint8_t param) const;
    // Builds/sends an RGB iLink command frame.
    esp_err_t sendRgbCmd(uint8_t r, uint8_t g, uint8_t b) const;

    // Clamps integer v to inclusive [lo..hi] and returns uint8_t.
    static uint8_t clampU8(int v, int lo, int hi);
    // Converts percentage 0..100 to byte range 0..255.
    static uint8_t percentTo255(uint8_t pct);
    // Computes iLink frame checksum byte.
    static uint8_t crc(const uint8_t *data, size_t len_without_crc);
};
