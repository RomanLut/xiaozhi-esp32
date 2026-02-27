#ifndef __ILINK_LAMP_CONTROLLER_H__
#define __ILINK_LAMP_CONTROLLER_H__

#include "mcp_server.h"
#include "sdkconfig.h"

#include <esp_log.h>
#include <cstring>
#include <string>

#if CONFIG_BT_ENABLED
#include "iLink.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

#define ILINK_TAG "ILinkLamp"

#if CONFIG_BT_ENABLED
class ILinkLampController {
private:
    volatile bool initialized_ = false;
    volatile bool has_lamp_ = false;
    esp_bd_addr_t cached_bda_ = {0};

    bool EnsureInitialized() {
        if (initialized_) return true;
        esp_err_t err = ILink::init();
        if (err != ESP_OK) {
            ESP_LOGE(ILINK_TAG, "ILink init failed: %s", esp_err_to_name(err));
            return false;
        }
        initialized_ = true;
        return true;
    }

    // Scans and caches the BDA of the first iLink lamp found. Returns false if none found.
    bool ScanAndCache() {
        ILink::LampInfo lamps[8];
        size_t count = ILink::getLamps(lamps, 8, 5000);
        for (size_t i = 0; i < count; i++) {
            if (lamps[i].has_a032) {
                memcpy(cached_bda_, lamps[i].bda, ESP_BD_ADDR_LEN);
                has_lamp_ = true;
                ESP_LOGI(ILINK_TAG, "Lamp cached: %s (rssi=%d)", lamps[i].name, lamps[i].rssi);
                return true;
            }
        }
        return false;
    }

    // Background task: init BLE + scan once at boot.
    static void DiscoveryTask(void* arg) {
        auto* self = static_cast<ILinkLampController*>(arg);
        if (self->EnsureInitialized()) {
            self->ScanAndCache();
        }
        vTaskDelete(nullptr);
    }

    // Connect → run command → disconnect. Uses cached BDA; falls back to live scan.
    std::string RunCommand(std::function<esp_err_t(ILink&)> cmd) {
        if (!EnsureInitialized()) {
            return "Failed to initialize Bluetooth";
        }
        if (!has_lamp_ && !ScanAndCache()) {
            return "No iLink lamp found nearby. Make sure the lamp is powered on.";
        }
        esp_bd_addr_t bda;
        memcpy(bda, cached_bda_, ESP_BD_ADDR_LEN);
        ILink lamp(bda);
        esp_err_t err = lamp.connect(10000);
        if (err != ESP_OK) {
            return std::string("Failed to connect to lamp: ") + esp_err_to_name(err);
        }
        err = cmd(lamp);
        vTaskDelay(pdMS_TO_TICKS(50)); // brief settle before disconnect
        lamp.disconnect();
        if (err != ESP_OK) {
            return std::string("Command failed: ") + esp_err_to_name(err);
        }
        return "";
    }

public:
    ILinkLampController() {
        // Discover lamp in background so first MCP call is fast.
        xTaskCreate(DiscoveryTask, "ilink_disc", 4096, this, 1, nullptr);

        auto& mcp = McpServer::GetInstance();

        mcp.AddTool("ilink.turn_on",
            "Turn on the light.",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                ESP_LOGI(ILINK_TAG, "MCP call: turn_on");
                auto err = RunCommand([](ILink& lamp) { return lamp.turnOn(); });
                if (!err.empty()) { ESP_LOGE(ILINK_TAG, "turn_on failed: %s", err.c_str()); return err; }
                ESP_LOGI(ILINK_TAG, "turn_on OK");
                return true;
            });

        mcp.AddTool("ilink.turn_off",
            "Turn off the light.",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                ESP_LOGI(ILINK_TAG, "MCP call: turn_off");
                auto err = RunCommand([](ILink& lamp) { return lamp.turnOff(); });
                if (!err.empty()) { ESP_LOGE(ILINK_TAG, "turn_off failed: %s", err.c_str()); return err; }
                ESP_LOGI(ILINK_TAG, "turn_off OK");
                return true;
            });

        mcp.AddTool("ilink.set_white",
            "Set the light temperature and brightness. brightness: 0-100 (0=off, 100=max). temperature: 0-100 (0=cool/cold white, 100=warm white).",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 80, 0, 100),
                Property("temperature", kPropertyTypeInteger, 50, 0, 100)
            }),
            [this](const PropertyList& props) -> ReturnValue {
                int b = props["brightness"].value<int>();
                int t = props["temperature"].value<int>();
                ESP_LOGI(ILINK_TAG, "MCP call: set_white brightness=%d temperature=%d", b, t);
                auto err = RunCommand([b, t](ILink& lamp) { return lamp.setWhite(b, t); });
                if (!err.empty()) { ESP_LOGE(ILINK_TAG, "set_white failed: %s", err.c_str()); return err; }
                ESP_LOGI(ILINK_TAG, "set_white OK");
                return true;
            });

        mcp.AddTool("ilink.set_brightness",
            "Set the light brightness. brightness: 0-100 (0=off, 100=max).",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 80, 0, 100)
            }),
            [this](const PropertyList& props) -> ReturnValue {
                int b = props["brightness"].value<int>();
                ESP_LOGI(ILINK_TAG, "MCP call: set_brightness brightness=%d", b);
                auto err = RunCommand([b](ILink& lamp) { return lamp.setBrightness(b); });
                if (!err.empty()) { ESP_LOGE(ILINK_TAG, "set_brightness failed: %s", err.c_str()); return err; }
                ESP_LOGI(ILINK_TAG, "set_brightness OK");
                return true;
            });

        mcp.AddTool("ilink.set_temperature",
            "Set the light color temperature. temperature: 0-100 (0=cool/cold white, 100=warm white).",
            PropertyList({
                Property("temperature", kPropertyTypeInteger, 50, 0, 100)
            }),
            [this](const PropertyList& props) -> ReturnValue {
                int t = props["temperature"].value<int>();
                ESP_LOGI(ILINK_TAG, "MCP call: set_temperature temperature=%d", t);
                auto err = RunCommand([t](ILink& lamp) { return lamp.setTemperature(t); });
                if (!err.empty()) { ESP_LOGE(ILINK_TAG, "set_temperature failed: %s", err.c_str()); return err; }
                ESP_LOGI(ILINK_TAG, "set_temperature OK");
                return true;
            });
    }
};
#else
class ILinkLampController {
public:
    ILinkLampController() {
        auto& mcp = McpServer::GetInstance();

        mcp.AddTool("ilink.turn_on",
            "Turn on the light.",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                return std::string("Bluetooth is disabled in this firmware build.");
            });

        mcp.AddTool("ilink.turn_off",
            "Turn off the light.",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                return std::string("Bluetooth is disabled in this firmware build.");
            });

        mcp.AddTool("ilink.set_white",
            "Set white mode. brightness: 0-100 (0=off, 100=max). temperature: 0-100 (0=cool/cold white, 100=warm white).",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 80, 0, 100),
                Property("temperature", kPropertyTypeInteger, 50, 0, 100)
            }),
            [](const PropertyList&) -> ReturnValue {
                return std::string("Bluetooth is disabled in this firmware build.");
            });

        mcp.AddTool("ilink.set_brightness",
            "Set the light brightness. brightness: 0-100 (0=off, 100=max).",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 80, 0, 100)
            }),
            [](const PropertyList&) -> ReturnValue {
                return std::string("Bluetooth is disabled in this firmware build.");
            });

        mcp.AddTool("ilink.set_temperature",
            "Set the light color temperature. temperature: 0-100 (0=cool/cold white, 100=warm white).",
            PropertyList({
                Property("temperature", kPropertyTypeInteger, 50, 0, 100)
            }),
            [](const PropertyList&) -> ReturnValue {
                return std::string("Bluetooth is disabled in this firmware build.");
            });
    }
};
#endif

#endif // __ILINK_LAMP_CONTROLLER_H__
