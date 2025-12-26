#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/gpio_rgb_led.h"
#include "assets/lang_config.h"
#include "device_state_event.h"
#include "board.h"

#include <wifi_station.h>
#include <http.h>
#include <cJSON.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_timer.h>
#include <esp_rom_sys.h>
#include <esp_sleep.h>

#define TAG "FluffyBoard"

class FluffyBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    Button touch_button_;
    esp_timer_handle_t idle_timer_ = nullptr;

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });
        touch_button_.OnPressDown([this]() {
            Application::GetInstance().StartListening();
        });
        touch_button_.OnPressUp([this]() {
            Application::GetInstance().StopListening();
        });
        touch_button_.OnDoubleClick([this]() {
            TurnOff();
        });
    }

    void InitializeIdleTimer() {
        // Initialize idle timer
        esp_timer_create_args_t idle_timer_args = {
            .callback = IdleTimerCallback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "idle_timer",
            .skip_unhandled_events = true
        };
        esp_timer_create(&idle_timer_args, &idle_timer_);
        RestartIdleTimer();
    }

    void RestartIdleTimer() {
        if (idle_timer_ != nullptr) {
            esp_timer_stop(idle_timer_);
            esp_timer_start_once(idle_timer_, IDLE_TIME_SECONDS * 1000000);
        }
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.turn_off", "Позволяет выключиться,когда пользователь это просит.", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI(TAG, "Device turning off from MCP");
            TurnOff();
            return true;
        });

        // Internet search tool - use this when LLM doesn't know something and needs to search for information
        mcp_server.AddTool("self.search_internet", 
            "Search for information on the internet using DuckDuckGo. Use this tool when you don't know the answer to a question or need up-to-date information.\n"
            "Args:\n"
            "  `query`: The search query to look up on the internet.\n"
            "Return:\n"
            "  Search results in JSON format with relevant information.",
            PropertyList({
                Property("query", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto query = properties["query"].value<std::string>();
                ESP_LOGI(TAG, "Searching internet for: %s", query.c_str());

                // URL encode the query
                std::string encoded_query;
                for (char c : query) {
                    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                        encoded_query += c;
                    } else if (c == ' ') {
                        encoded_query += '+';
                    } else {
                        char hex[4];
                        snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c);
                        encoded_query += hex;
                    }
                }

                // Use DuckDuckGo Instant Answer API
                std::string url = "https://api.duckduckgo.com/?q=" + encoded_query + "&format=json&no_html=1&skip_disambig=1";
                
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(10);
                if (!http->Open("GET", url)) {
                    ESP_LOGE(TAG, "Failed to open HTTP connection for search");
                    return std::string("{\"error\": \"Failed to connect to search service\"}");
                }

                int status_code = http->GetStatusCode();
                if (status_code != 200) {
                    ESP_LOGE(TAG, "Search request failed with status: %d", status_code);
                    http->Close();
                    return std::string("{\"error\": \"Search request failed with status " + std::to_string(status_code) + "\"}");
                }

                std::string response = http->ReadAll();
                http->Close();

                // Parse the DuckDuckGo response and extract useful information
                cJSON* json = cJSON_Parse(response.c_str());
                if (json == nullptr) {
                    ESP_LOGE(TAG, "Failed to parse search response");
                    return std::string("{\"error\": \"Failed to parse search response\"}");
                }

                cJSON* result = cJSON_CreateObject();
                cJSON_AddStringToObject(result, "query", query.c_str());

                // Extract Abstract (main answer)
                cJSON* abstract_text = cJSON_GetObjectItem(json, "AbstractText");
                if (cJSON_IsString(abstract_text) && strlen(abstract_text->valuestring) > 0) {
                    cJSON_AddStringToObject(result, "answer", abstract_text->valuestring);
                }

                // Extract Abstract Source
                cJSON* abstract_source = cJSON_GetObjectItem(json, "AbstractSource");
                if (cJSON_IsString(abstract_source) && strlen(abstract_source->valuestring) > 0) {
                    cJSON_AddStringToObject(result, "source", abstract_source->valuestring);
                }

                // Extract Abstract URL
                cJSON* abstract_url = cJSON_GetObjectItem(json, "AbstractURL");
                if (cJSON_IsString(abstract_url) && strlen(abstract_url->valuestring) > 0) {
                    cJSON_AddStringToObject(result, "url", abstract_url->valuestring);
                }

                // Extract Related Topics for additional context
                cJSON* related_topics = cJSON_GetObjectItem(json, "RelatedTopics");
                if (cJSON_IsArray(related_topics) && cJSON_GetArraySize(related_topics) > 0) {
                    cJSON* topics_array = cJSON_CreateArray();
                    int count = 0;
                    cJSON* topic = nullptr;
                    cJSON_ArrayForEach(topic, related_topics) {
                        if (count >= 3) break;  // Limit to 3 related topics
                        cJSON* text = cJSON_GetObjectItem(topic, "Text");
                        if (cJSON_IsString(text) && strlen(text->valuestring) > 0) {
                            cJSON_AddItemToArray(topics_array, cJSON_CreateString(text->valuestring));
                            count++;
                        }
                    }
                    if (cJSON_GetArraySize(topics_array) > 0) {
                        cJSON_AddItemToObject(result, "related", topics_array);
                    } else {
                        cJSON_Delete(topics_array);
                    }
                }

                // Extract Infobox if available
                cJSON* infobox = cJSON_GetObjectItem(json, "Infobox");
                if (cJSON_IsObject(infobox)) {
                    cJSON* content = cJSON_GetObjectItem(infobox, "content");
                    if (cJSON_IsArray(content) && cJSON_GetArraySize(content) > 0) {
                        cJSON* info_array = cJSON_CreateArray();
                        int count = 0;
                        cJSON* item = nullptr;
                        cJSON_ArrayForEach(item, content) {
                            if (count >= 5) break;  // Limit to 5 infobox items
                            cJSON* label = cJSON_GetObjectItem(item, "label");
                            cJSON* value = cJSON_GetObjectItem(item, "value");
                            if (cJSON_IsString(label) && cJSON_IsString(value)) {
                                cJSON* info_item = cJSON_CreateObject();
                                cJSON_AddStringToObject(info_item, "label", label->valuestring);
                                cJSON_AddStringToObject(info_item, "value", value->valuestring);
                                cJSON_AddItemToArray(info_array, info_item);
                                count++;
                            }
                        }
                        if (cJSON_GetArraySize(info_array) > 0) {
                            cJSON_AddItemToObject(result, "info", info_array);
                        } else {
                            cJSON_Delete(info_array);
                        }
                    }
                }

                cJSON_Delete(json);

                char* result_str = cJSON_PrintUnformatted(result);
                std::string result_string(result_str);
                cJSON_free(result_str);
                cJSON_Delete(result);

                ESP_LOGI(TAG, "Search result: %s", result_string.c_str());
                return result_string;
            });
    }

    void OnDeviceStateChanged(DeviceState previous_state, DeviceState current_state) {
        if (current_state == kDeviceStateSpeaking) {
            // Reset the idle timer when entering speaking state
            gpio_set_level(KEEP_ON_PIN, 1);  // Ensure keep-on pin is high
            RestartIdleTimer();
        }
    }

    void TurnOff() {
        gpio_set_level(KEEP_ON_PIN, 0);  // Set keep-on pin low after IDLE_TIME_SECONDS of no speaking
        static_cast<GpioRGBLed*>(GetLed())->SetBrightness(5);
        ESP_LOGI(TAG, "Device idle for %d seconds, setting KEEP_ON_PIN low", IDLE_TIME_SECONDS);
        esp_rom_delay_us(100000);  // Busy wait for 100ms
    }

    static void IdleTimerCallback(void* arg) {
        ((FluffyBoard*)arg)->TurnOff();
    }

public:
    FluffyBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        touch_button_(TOUCH_BUTTON_GPIO) {

        gpio_set_direction(KEEP_ON_PIN, GPIO_MODE_OUTPUT);
        gpio_set_level(KEEP_ON_PIN, 1);
        
        InitializeButtons();

        InitializeIdleTimer();

        // Register device state change callback
        DeviceStateEventManager::GetInstance().RegisterStateChangeCallback(
            [this](DeviceState previous_state, DeviceState current_state) {
                OnDeviceStateChanged(previous_state, current_state);
            }
        );

        InitializeTools();

    }

    virtual Led* GetLed() override {
        static GpioRGBLed led(R_LED_GPIO, G_LED_GPIO, B_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
/*
        static NoAudioCodecSimplexPdm audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_DIN);
*/            

        static NoAudioCodecPdmWithRef audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_DIN);

        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        static Display* display_ = new NoDisplay();
        return display_;
    }

    ~FluffyBoard() {
        if (idle_timer_ != nullptr) {
            esp_timer_stop(idle_timer_);
            esp_timer_delete(idle_timer_);
        }
    }
};

DECLARE_BOARD(FluffyBoard);
