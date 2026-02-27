#include "iLink.h"

#include <stdlib.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gattc_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

namespace {

static const char *TAG = "iLinkLib";
static constexpr uint16_t ILINK_SERVICE_UUID16 = 0xA032;
static constexpr uint16_t ILINK_CHAR_CMD_UUID16 = 0xA040;
static constexpr uint16_t ILINK_APP_ID = 0;

static constexpr EventBits_t EVT_SCAN_DONE = BIT0;
static constexpr EventBits_t EVT_CONNECT_READY = BIT1;
static constexpr EventBits_t EVT_CONNECT_FAIL = BIT2;

static esp_ble_scan_params_t s_scan_params = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50,
    .scan_window = 0x30,
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
};

struct Backend {
    bool initialized = false;
    bool app_registered = false;
    bool scan_params_ready = false;
    bool scanning = false;
    bool discover_mode = false;
    bool connect_mode = false;
    bool connecting = false;
    bool connected = false;
    bool service_found = false;
    bool ready = false;

    esp_gatt_if_t gattc_if = ESP_GATT_IF_NONE;
    uint16_t conn_id = 0;
    esp_bd_addr_t connected_bda = {0};
    esp_bd_addr_t target_bda = {0};
    uint16_t service_start = 0;
    uint16_t service_end = 0;
    uint16_t cmd_char_handle = 0;

    ILink::LampInfo discovered[16] = {};
    size_t discovered_count = 0;

    EventGroupHandle_t events = nullptr;
    SemaphoreHandle_t write_done = nullptr;
};

static Backend s;

static bool bda_equal(const esp_bd_addr_t a, const esp_bd_addr_t b)
{
    return memcmp(a, b, ESP_BD_ADDR_LEN) == 0;
}

static void copy_bda(esp_bd_addr_t dst, const esp_bd_addr_t src)
{
    memcpy(dst, src, ESP_BD_ADDR_LEN);
}

static void log_bda(const char *prefix, const esp_bd_addr_t bda)
{
    ESP_LOGI(TAG, "%s %02X:%02X:%02X:%02X:%02X:%02X",
             prefix, bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

static void parse_adv(const uint8_t *raw, uint8_t raw_len, char *name_out, size_t name_out_len, bool *has_a032_out)
{
    if (name_out_len > 0) {
        name_out[0] = '\0';
    }
    *has_a032_out = false;

    uint8_t idx = 0;
    while (idx + 1 < raw_len) {
        uint8_t field_len = raw[idx];
        if (field_len == 0 || (idx + 1 + field_len) > raw_len) {
            break;
        }
        uint8_t ad_type = raw[idx + 1];
        const uint8_t *ad_data = &raw[idx + 2];
        uint8_t ad_data_len = (uint8_t)(field_len - 1);

        if ((ad_type == 0x08 || ad_type == 0x09) && ad_data_len > 0 && name_out_len > 0) {
            size_t n = ad_data_len < (name_out_len - 1) ? ad_data_len : (name_out_len - 1);
            memcpy(name_out, ad_data, n);
            name_out[n] = '\0';
        }

        if (ad_type == 0x02 || ad_type == 0x03) {
            for (uint8_t i = 0; i + 1 < ad_data_len; i += 2) {
                uint16_t uuid16 = (uint16_t)(ad_data[i] | (ad_data[i + 1] << 8));
                if (uuid16 == ILINK_SERVICE_UUID16) {
                    *has_a032_out = true;
                }
            }
        }
        idx = (uint8_t)(idx + field_len + 1);
    }
}

static int find_discovered_slot(const esp_bd_addr_t bda)
{
    for (size_t i = 0; i < s.discovered_count; ++i) {
        if (bda_equal(s.discovered[i].bda, bda)) {
            return (int)i;
        }
    }
    return -1;
}

static void upsert_discovered(const esp_bd_addr_t bda, int8_t rssi, const char *name, bool has_a032)
{
    int idx = find_discovered_slot(bda);
    if (idx < 0) {
        if (s.discovered_count >= (sizeof(s.discovered) / sizeof(s.discovered[0]))) {
            return;
        }
        idx = (int)s.discovered_count++;
        memset(&s.discovered[idx], 0, sizeof(s.discovered[idx]));
        copy_bda(s.discovered[idx].bda, bda);
    }

    ILink::LampInfo &lamp = s.discovered[idx];
    lamp.rssi = rssi;
    lamp.has_a032 = lamp.has_a032 || has_a032;
    if (name != nullptr && name[0] != '\0') {
        strncpy(lamp.name, name, sizeof(lamp.name) - 1);
        lamp.name[sizeof(lamp.name) - 1] = '\0';
    }
}

static bool looks_like_ilink_candidate(const char *name, bool has_a032)
{
    return has_a032 || (name != nullptr && strstr(name, "iLink") != nullptr);
}

static void start_scan(uint32_t duration_sec)
{
    if (!s.scan_params_ready || s.scanning) {
        return;
    }
    esp_err_t err = esp_ble_gap_start_scanning(duration_sec);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start scan failed: %s", esp_err_to_name(err));
    }
}

static void stop_scan()
{
    if (!s.scanning) {
        return;
    }
    esp_ble_gap_stop_scanning();
}

static void reset_connection_state()
{
    s.connecting = false;
    s.connected = false;
    s.service_found = false;
    s.ready = false;
    s.service_start = 0;
    s.service_end = 0;
    s.cmd_char_handle = 0;
}

static void signal_connect_fail()
{
    xEventGroupSetBits(s.events, EVT_CONNECT_FAIL);
}

static void signal_connect_ready()
{
    xEventGroupSetBits(s.events, EVT_CONNECT_READY);
}

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        s.scan_params_ready = true;
        break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s.scanning = true;
        } else {
            ESP_LOGE(TAG, "scan start failed: %d", param->scan_start_cmpl.status);
            if (s.discover_mode) {
                xEventGroupSetBits(s.events, EVT_SCAN_DONE);
            }
            if (s.connect_mode) {
                signal_connect_fail();
            }
        }
        break;

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        s.scanning = false;
        if (s.discover_mode) {
            xEventGroupSetBits(s.events, EVT_SCAN_DONE);
        }
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
            s.scanning = false;
            if (s.discover_mode) {
                xEventGroupSetBits(s.events, EVT_SCAN_DONE);
            }
            break;
        }

        if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) {
            break;
        }

        const uint8_t *raw = param->scan_rst.ble_adv;
        uint8_t raw_len = param->scan_rst.adv_data_len + param->scan_rst.scan_rsp_len;
        if (raw_len == 0) {
            break;
        }

        char name[32] = {0};
        bool has_a032 = false;
        parse_adv(raw, raw_len, name, sizeof(name), &has_a032);

        if (looks_like_ilink_candidate(name, has_a032)) {
            upsert_discovered(param->scan_rst.bda, param->scan_rst.rssi, name, has_a032);
        }

        if (s.connect_mode && !s.connecting && !s.connected && bda_equal(param->scan_rst.bda, s.target_bda)) {
            ESP_LOGI(TAG, "Found target lamp, connecting...");
            log_bda("Target", s.target_bda);
            s.connecting = true;
            stop_scan();
            esp_err_t err = esp_ble_gattc_open(s.gattc_if, param->scan_rst.bda, param->scan_rst.ble_addr_type, true);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "gattc_open failed: %s", esp_err_to_name(err));
                s.connecting = false;
                signal_connect_fail();
            }
        }
        break;
    }

    default:
        break;
    }
}

static void gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    if (event == ESP_GATTC_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            s.gattc_if = gattc_if;
            s.app_registered = true;
        }
        return;
    }

    if (gattc_if != ESP_GATT_IF_NONE && s.gattc_if != ESP_GATT_IF_NONE && gattc_if != s.gattc_if) {
        return;
    }

    switch (event) {
    case ESP_GATTC_OPEN_EVT:
        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "open failed status=0x%x", param->open.status);
            reset_connection_state();
            signal_connect_fail();
            break;
        }
        s.connecting = false;
        s.connected = true;
        s.conn_id = param->open.conn_id;
        copy_bda(s.connected_bda, param->open.remote_bda);
        esp_ble_gattc_send_mtu_req(s.gattc_if, s.conn_id);
        break;

    case ESP_GATTC_CFG_MTU_EVT:
        if (param->cfg_mtu.status != ESP_GATT_OK) {
            signal_connect_fail();
            break;
        }
        esp_ble_gattc_search_service(s.gattc_if, s.conn_id, nullptr);
        break;

    case ESP_GATTC_SEARCH_RES_EVT:
        if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
            param->search_res.srvc_id.uuid.uuid.uuid16 == ILINK_SERVICE_UUID16) {
            s.service_found = true;
            s.service_start = param->search_res.start_handle;
            s.service_end = param->search_res.end_handle;
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT: {
        if (param->search_cmpl.status != ESP_GATT_OK || !s.service_found) {
            signal_connect_fail();
            break;
        }
        uint16_t count = 0;
        esp_bt_uuid_t char_uuid = {
            .len = ESP_UUID_LEN_16,
            .uuid = {.uuid16 = ILINK_CHAR_CMD_UUID16},
        };
        esp_gatt_status_t st = esp_ble_gattc_get_attr_count(
            s.gattc_if, s.conn_id, ESP_GATT_DB_CHARACTERISTIC,
            s.service_start, s.service_end, ESP_GATT_ILLEGAL_HANDLE, &count);
        if (st != ESP_GATT_OK || count == 0) {
            signal_connect_fail();
            break;
        }
        esp_gattc_char_elem_t *chars = (esp_gattc_char_elem_t *)calloc(count, sizeof(*chars));
        if (chars == nullptr) {
            signal_connect_fail();
            break;
        }
        st = esp_ble_gattc_get_char_by_uuid(
            s.gattc_if, s.conn_id, s.service_start, s.service_end, char_uuid, chars, &count);
        if (st != ESP_GATT_OK || count == 0) {
            free(chars);
            signal_connect_fail();
            break;
        }
        s.cmd_char_handle = chars[0].char_handle;
        free(chars);
        s.ready = true;
        signal_connect_ready();
        break;
    }

    case ESP_GATTC_DISCONNECT_EVT:
        reset_connection_state();
        if (s.connect_mode) {
            signal_connect_fail();
        }
        break;

    case ESP_GATTC_WRITE_CHAR_EVT:
        if (param->write.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "write failed status=0x%x", param->write.status);
        }
        if (s.write_done) {
            xSemaphoreGive(s.write_done);
        }
        break;

    default:
        break;
    }
}

} // namespace

esp_err_t ILink::init()
{
    if (s.initialized) {
        return ESP_OK;
    }

    s.events = xEventGroupCreate();
    if (s.events == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    s.write_done = xSemaphoreCreateBinary();
    if (s.write_done == nullptr) {
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_IDF_TARGET_ESP32
    // ESP32 has a dual-mode controller; release Classic BT memory since we use BLE only.
    {
        esp_err_t rel_err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        if (rel_err != ESP_OK && rel_err != ESP_ERR_INVALID_STATE) {
            return rel_err;
        }
    }
#endif

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_bluedroid_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_bluedroid_enable();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    ESP_RETURN_ON_ERROR(esp_ble_gap_register_callback(gap_cb), TAG, "gap cb reg failed");
    ESP_RETURN_ON_ERROR(esp_ble_gattc_register_callback(gattc_cb), TAG, "gattc cb reg failed");
    ESP_RETURN_ON_ERROR(esp_ble_gattc_app_register(ILINK_APP_ID), TAG, "gattc app reg failed");
    esp_ble_gatt_set_local_mtu(128);
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_scan_params(&s_scan_params), TAG, "scan params failed");

    for (int i = 0; i < 50 && (!s.app_registered || !s.scan_params_ready); ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s.app_registered || !s.scan_params_ready) {
        return ESP_ERR_TIMEOUT;
    }

    s.initialized = true;
    return ESP_OK;
}

size_t ILink::getLamps(LampInfo *out_lamps, size_t max_lamps, uint32_t scan_ms)
{
    if (out_lamps == nullptr || max_lamps == 0) {
        return 0;
    }
    if (!s.initialized) {
        return 0;
    }

    s.discover_mode = true;
    s.connect_mode = false;
    s.discovered_count = 0;
    xEventGroupClearBits(s.events, EVT_SCAN_DONE | EVT_CONNECT_READY | EVT_CONNECT_FAIL);

    uint32_t secs = (scan_ms + 999u) / 1000u;
    if (secs == 0) {
        secs = 1;
    }
    start_scan(secs);

    xEventGroupWaitBits(s.events, EVT_SCAN_DONE, pdTRUE, pdFALSE, pdMS_TO_TICKS(scan_ms + 1500));
    s.discover_mode = false;

    size_t count = s.discovered_count < max_lamps ? s.discovered_count : max_lamps;
    for (size_t i = 0; i < count; ++i) {
        out_lamps[i] = s.discovered[i];
    }
    return count;
}

ILink::ILink(const esp_bd_addr_t bda) : target_bda_{0}
{
    if (bda != nullptr) {
        memcpy(target_bda_, bda, ESP_BD_ADDR_LEN);
    }
}

bool ILink::matchesTargetBda(const esp_bd_addr_t bda) const
{
    return bda != nullptr && memcmp(target_bda_, bda, ESP_BD_ADDR_LEN) == 0;
}

const uint8_t *ILink::targetBda() const
{
    return target_bda_;
}

esp_err_t ILink::connect(uint32_t timeout_ms)
{
    if (!s.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s.ready && bda_equal(s.connected_bda, target_bda_)) {
        return ESP_OK;
    }

    if (s.connected && !bda_equal(s.connected_bda, target_bda_)) {
        esp_ble_gattc_close(s.gattc_if, s.conn_id);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    reset_connection_state();
    copy_bda(s.target_bda, target_bda_);
    s.connect_mode = true;
    xEventGroupClearBits(s.events, EVT_CONNECT_READY | EVT_CONNECT_FAIL | EVT_SCAN_DONE);
    start_scan(0);

    EventBits_t bits = xEventGroupWaitBits(
        s.events, EVT_CONNECT_READY | EVT_CONNECT_FAIL, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    s.connect_mode = false;
    if (bits & EVT_CONNECT_READY) {
        return ESP_OK;
    }
    stop_scan();
    return ESP_ERR_TIMEOUT;
}

bool ILink::isReady() const
{
    return s.ready && bda_equal(s.connected_bda, target_bda_);
}

esp_err_t ILink::disconnect()
{
    if (s.connected && bda_equal(s.connected_bda, target_bda_)) {
        return esp_ble_gattc_close(s.gattc_if, s.conn_id);
    }
    return ESP_OK;
}

esp_err_t ILink::write(const uint8_t *data, uint16_t len) const
{
    if (!isReady()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_ble_gattc_write_char(
        s.gattc_if, s.conn_id, s.cmd_char_handle, len, (uint8_t *)data,
        ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
    if (err == ESP_OK && s.write_done) {
        xSemaphoreTake(s.write_done, pdMS_TO_TICKS(3000));
    }
    return err;
}

uint8_t ILink::clampU8(int v, int lo, int hi)
{
    if (v < lo) {
        return (uint8_t)lo;
    }
    if (v > hi) {
        return (uint8_t)hi;
    }
    return (uint8_t)v;
}

uint8_t ILink::percentTo255(uint8_t pct)
{
    uint8_t p = clampU8(pct, 0, 100);
    return (uint8_t)((p * 255 + 50) / 100);
}

uint8_t ILink::crc(const uint8_t *data, size_t len_without_crc)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len_without_crc; ++i) {
        sum += data[i];
    }
    return (uint8_t)((0xFFu - sum) & 0xFFu);
}

esp_err_t ILink::sendStdCmd(uint8_t cmd_hi, uint8_t cmd_lo, uint8_t param) const
{
    uint8_t buf[7] = {0x55, 0xAA, 0x01, cmd_hi, cmd_lo, param, 0x00};
    buf[6] = crc(buf, 6);
    return write(buf, sizeof(buf));
}

esp_err_t ILink::sendRgbCmd(uint8_t r, uint8_t g, uint8_t b) const
{
    uint8_t buf[9] = {0x55, 0xAA, 0x03, 0x08, 0x02, r, g, b, 0x00};
    buf[8] = crc(buf, 8);
    return write(buf, sizeof(buf));
}

esp_err_t ILink::turnOn()
{
    return sendStdCmd(0x08, 0x05, 0x01);
}

esp_err_t ILink::turnOff()
{
    return sendStdCmd(0x08, 0x05, 0x00);
}

esp_err_t ILink::setWhite(uint8_t brightness_percent, uint8_t temperature_percent)
{
    uint8_t b_pct = clampU8(brightness_percent, 0, 100);
    uint8_t t_pct = clampU8(temperature_percent, 0, 100);

    uint8_t temp_level = (uint8_t)(1 + ((t_pct * 4 + 50) / 100));
    temp_level = clampU8(temp_level, 1, 5);

    uint8_t dim = percentTo255(b_pct);
    if (dim == 0) {
        dim = 1;
    }

    esp_err_t err = sendStdCmd(0x08, 0x09, temp_level);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(40));
    return sendStdCmd(0x08, 0x01, dim);
}

esp_err_t ILink::setBrightness(uint8_t brightness_percent)
{
    uint8_t dim = percentTo255(clampU8(brightness_percent, 0, 100));
    if (dim == 0) {
        dim = 1;
    }
    return sendStdCmd(0x08, 0x01, dim);
}

esp_err_t ILink::setTemperature(uint8_t temperature_percent)
{
    uint8_t t_pct = clampU8(temperature_percent, 0, 100);
    uint8_t temp_level = (uint8_t)(1 + ((t_pct * 4 + 50) / 100));
    temp_level = clampU8(temp_level, 1, 5);
    return sendStdCmd(0x08, 0x09, temp_level);
}

esp_err_t ILink::setRGB(uint8_t r_percent, uint8_t g_percent, uint8_t b_percent)
{
    return sendRgbCmd(percentTo255(r_percent), percentTo255(g_percent), percentTo255(b_percent));
}
