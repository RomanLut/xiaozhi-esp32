#include "ilink_lamp_controller.h"

#include <esp_log.h>

#include <cstring>

namespace {
static constexpr uint32_t kScanDurationMs = 5000;
static constexpr uint32_t kRetryDelayMs = 5000;
static constexpr uint32_t kPeriodicDelayMs = 30000;
static constexpr int kInitialRetryCount = 3;
} // namespace

bool ILinkLampController::EnsureInitialized() {
    if (initialized_) {
        return true;
    }

    esp_err_t err = ILink::init();
    if (err != ESP_OK) {
        ESP_LOGE(ILINK_TAG, "ILink init failed: %s", esp_err_to_name(err));
        return false;
    }

    initialized_ = true;
    return true;
}

bool ILinkLampController::ScanAndCache() {
    ILink::LampInfo lamps[8];
    size_t count = ILink::getLamps(lamps, 8, kScanDurationMs);

    int best_idx = -1;
    int8_t best_rssi = INT8_MIN;
    for (size_t i = 0; i < count; ++i) {
        if (!lamps[i].has_a032) {
            continue;
        }
        if (best_idx < 0 || lamps[i].rssi > best_rssi) {
            best_idx = static_cast<int>(i);
            best_rssi = lamps[i].rssi;
        }
    }

    if (best_idx < 0) {
        has_lamp_ = false;
        memset(cached_bda_, 0, sizeof(cached_bda_));
        ESP_LOGI(ILINK_TAG, "No iLink lamp found during scan");
        return false;
    }

    memcpy(cached_bda_, lamps[best_idx].bda, ESP_BD_ADDR_LEN);
    has_lamp_ = true;
    ESP_LOGI(ILINK_TAG, "Lamp cached: %s (rssi=%d)", lamps[best_idx].name, lamps[best_idx].rssi);
    return true;
}

void ILinkLampController::DiscoveryTask(void* arg) {
    auto* self = static_cast<ILinkLampController*>(arg);
    int failed_scans = 0;

    while (!self->stop_task_) {
        bool found = false;

        if (xSemaphoreTake(self->ble_mutex_, portMAX_DELAY) == pdTRUE) {
            if (!self->stop_task_ && self->EnsureInitialized()) {
                found = self->ScanAndCache();
            }
            xSemaphoreGive(self->ble_mutex_);
        }

        if (self->stop_task_) {
            break;
        }

        uint32_t delay_ms = kPeriodicDelayMs;
        if (found) {
            failed_scans = 0;
        } else {
            ++failed_scans;
            if (failed_scans <= kInitialRetryCount) {
                delay_ms = kRetryDelayMs;
            }
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms));
    }

    self->discovery_task_ = nullptr;
    vTaskDelete(nullptr);
}

std::string ILinkLampController::RunCommand(std::function<esp_err_t(ILink&)> cmd) {
    if (xSemaphoreTake(ble_mutex_, portMAX_DELAY) != pdTRUE) {
        return "Failed to lock Bluetooth controller";
    }

    if (!EnsureInitialized()) {
        xSemaphoreGive(ble_mutex_);
        return "Failed to initialize Bluetooth";
    }
    if (!has_lamp_) {
        xSemaphoreGive(ble_mutex_);
        return "No iLink lamp found nearby. Make sure the lamp is powered on.";
    }

    esp_bd_addr_t bda;
    memcpy(bda, cached_bda_, ESP_BD_ADDR_LEN);
    ILink lamp(bda);
    esp_err_t err = lamp.connect(10000);
    if (err != ESP_OK) {
        xSemaphoreGive(ble_mutex_);
        return std::string("Failed to connect to lamp: ") + esp_err_to_name(err);
    }

    err = cmd(lamp);
    vTaskDelay(pdMS_TO_TICKS(50));
    lamp.disconnect();
    xSemaphoreGive(ble_mutex_);

    if (err != ESP_OK) {
        return std::string("Command failed: ") + esp_err_to_name(err);
    }
    return "";
}

ILinkLampController::ILinkLampController() {
    ble_mutex_ = xSemaphoreCreateMutex();
    if (ble_mutex_ == nullptr) {
        ESP_LOGE(ILINK_TAG, "Failed to create BLE mutex");
        return;
    }

    BaseType_t created = xTaskCreate(DiscoveryTask, "ilink_disc", 4096, this, 1, &discovery_task_);
    if (created != pdPASS) {
        ESP_LOGE(ILINK_TAG, "Failed to create discovery task");
        discovery_task_ = nullptr;
    }

    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("ilink.turn_on",
        "Turn on the light.",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            ESP_LOGI(ILINK_TAG, "MCP call: turn_on");
            auto err = RunCommand([](ILink& lamp) { return lamp.turnOn(); });
            if (!err.empty()) {
                ESP_LOGE(ILINK_TAG, "turn_on failed: %s", err.c_str());
                return err;
            }
            ESP_LOGI(ILINK_TAG, "turn_on OK");
            return true;
        });

    mcp.AddTool("ilink.turn_off",
        "Turn off the light.",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            ESP_LOGI(ILINK_TAG, "MCP call: turn_off");
            auto err = RunCommand([](ILink& lamp) { return lamp.turnOff(); });
            if (!err.empty()) {
                ESP_LOGE(ILINK_TAG, "turn_off failed: %s", err.c_str());
                return err;
            }
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
            if (!err.empty()) {
                ESP_LOGE(ILINK_TAG, "set_white failed: %s", err.c_str());
                return err;
            }
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
            if (!err.empty()) {
                ESP_LOGE(ILINK_TAG, "set_brightness failed: %s", err.c_str());
                return err;
            }
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
            if (!err.empty()) {
                ESP_LOGE(ILINK_TAG, "set_temperature failed: %s", err.c_str());
                return err;
            }
            ESP_LOGI(ILINK_TAG, "set_temperature OK");
            return true;
        });
}

ILinkLampController::~ILinkLampController() {
    stop_task_ = true;
    if (discovery_task_ != nullptr) {
        xTaskNotifyGive(discovery_task_);
        while (discovery_task_ != nullptr) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (ble_mutex_ != nullptr) {
        vSemaphoreDelete(ble_mutex_);
        ble_mutex_ = nullptr;
    }
}
