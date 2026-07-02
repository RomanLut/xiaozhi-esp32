#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "settings.h"
#include "mcp_server.h"
#include "ilink_lamp_controller.h"
#include "led/gpio_rgb_led.h"
#include "assets/lang_config.h"
#include "device_state_event.h"
#include "board.h"
#include "local_config.h"

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
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_err.h>

#include <algorithm>
#include <array>

#define TAG "FluffyBoard"

class FluffyBoard : public WifiBoard {
private:
    static constexpr adc_channel_t kBatteryAdcChannel = ADC_CHANNEL_5;  // GPIO6 on ESP32-S3
    static constexpr adc_atten_t kBatteryAdcAtten = ADC_ATTEN_DB_12;
    static constexpr adc_bitwidth_t kBatteryAdcBitwidth = ADC_BITWIDTH_12;
    static constexpr size_t kAdcBurstSamples = 8;
    static constexpr size_t kAdcTrimEachSide = 5;
    static constexpr size_t kBatteryAverageSamples = 10;
    static constexpr float kBatteryVoltageScale = 1.5f;  // 10k(top)/20k(bottom): Vbat = Vgpio * (10k+20k)/20k
    static constexpr uint16_t kTouchPressedMinMv = 0;
    static constexpr uint16_t kTouchPressedMaxMv = 1000;  // <1.0V on GPIO6 means button pressed

    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    Button* touch_button_ = nullptr;
    adc_oneshot_unit_handle_t shared_adc_handle_ = nullptr;
    adc_cali_handle_t adc_cali_handle_ = nullptr;
    bool adc_calibration_enabled_ = false;
    esp_timer_handle_t idle_timer_ = nullptr;
    esp_timer_handle_t battery_timer_ = nullptr;
    std::array<int, kBatteryAverageSamples> battery_samples_{};
    size_t battery_sample_count_ = 0;
    size_t battery_sample_index_ = 0;
    int64_t battery_sample_sum_ = 0;
    ILinkLampController* ilink_lamp_controller_ = nullptr;

    void InitializeSharedAdc() {
        adc_oneshot_unit_init_cfg_t unit_cfg = {
            .unit_id = ADC_UNIT_1,
            .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &shared_adc_handle_));
        adc_oneshot_chan_cfg_t chan_cfg = {
            .atten = kBatteryAdcAtten,
            .bitwidth = kBatteryAdcBitwidth,
        };
        ESP_ERROR_CHECK(adc_oneshot_config_channel(shared_adc_handle_, kBatteryAdcChannel, &chan_cfg));
        ESP_LOGI(TAG, "GPIO6 ADC configured: atten=ADC_ATTEN_DB_12, bitwidth=%d", static_cast<int>(kBatteryAdcBitwidth));

        // GPIO6 is used as ADC input for both battery sensing and ADC button.
        ESP_ERROR_CHECK(gpio_set_pull_mode(TOUCH_BUTTON_GPIO, GPIO_FLOATING));
    }

    void InitializeAdcCalibration() {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = ADC_UNIT_1,
            .atten = kBatteryAdcAtten,
            .bitwidth = kBatteryAdcBitwidth,
        };
        esp_err_t ret = adc_cali_create_scheme_curve_fitting(&cali_cfg, &adc_cali_handle_);
        if (ret == ESP_OK) {
            adc_calibration_enabled_ = true;
            ESP_LOGI(TAG, "ADC calibration enabled (curve fitting)");
            return;
        }
        ESP_LOGW(TAG, "ADC calibration unavailable, fallback to approximate conversion: %s", esp_err_to_name(ret));
#else
        ESP_LOGW(TAG, "ADC curve fitting calibration not supported, fallback to approximate conversion");
#endif
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });

        button_adc_config_t adc_cfg = {};
        adc_cfg.adc_handle = &shared_adc_handle_;
        adc_cfg.unit_id = ADC_UNIT_1;
        adc_cfg.adc_channel = static_cast<uint8_t>(kBatteryAdcChannel);
        adc_cfg.button_index = 0;
        adc_cfg.min = kTouchPressedMinMv;
        adc_cfg.max = kTouchPressedMaxMv;
        touch_button_ = new AdcButton(adc_cfg);

        touch_button_->OnPressDown([this]() {
            Application::GetInstance().StartListening();
        });
        touch_button_->OnPressUp([this]() {
            Application::GetInstance().StopListening();
        });
        touch_button_->OnDoubleClick([this]() {
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

    void InitializeBatteryMonitor() {
        esp_timer_create_args_t battery_timer_args = {
            .callback = BatteryTimerCallback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "battery_timer",
            .skip_unhandled_events = true
        };
        ESP_ERROR_CHECK(esp_timer_create(&battery_timer_args, &battery_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(battery_timer_, 1000000));  // 1 second
    }

    void RestartIdleTimer() {
        if (idle_timer_ != nullptr) {
            esp_timer_stop(idle_timer_);
            esp_timer_start_once(idle_timer_, IDLE_TIME_SECONDS * 1000000);
        }
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.turn_off", "Если пользователь просить отключиться или выключиться, нужно вызвать эту команду.", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI(TAG, "Device turning off from MCP");
            TurnOff();
            return true;
        });

        mcp_server.AddTool("self.system.reboot", "Reboot the device.", PropertyList(), [](const PropertyList& properties) -> ReturnValue {
            ESP_LOGI(TAG, "Device reboot requested from MCP");
            Application::GetInstance().Reboot();
            return true;
        });
        // Internet search tool - use this when LLM doesn't know something and needs to search for information
        mcp_server.AddTool("self.search_internet",
            "Search for information on the internet. Use this tool when you don't know the answer to a question or need up-to-date information. Call this tool immediately without generating any prior response.\n"
            "Args:\n"
            "  `query`: The search query to look up on the internet.\n"
            "Return:\n"
            "  The answer to the query.",
            PropertyList({
                Property("query", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto query = properties["query"].value<std::string>();
                ESP_LOGI(TAG, "Searching internet for: %s", query.c_str());

                //indicate using LED
                GpioRGBLed* led = static_cast<GpioRGBLed*>(GetLed());
                led->SetBrightness(HIGH_BRIGHTNESS);
                led->SetColor(255, 0, 255); 
                led->StartContinuousBlink(100);

                // Get Tavily API key from settings or local config
                Settings settings("app", false);
                std::string api_key = settings.GetString("tavily_api_key", TAVILY_API_KEY);
                if (api_key.empty()) {
                    ESP_LOGE(TAG, "Tavily API key not found in settings");
                    return std::string("Tavily API key not configured");
                }

                // Create JSON request body
                cJSON* request_json = cJSON_CreateObject();
                cJSON_AddStringToObject(request_json, "query", query.c_str());
                cJSON_AddStringToObject(request_json, "include_answer", "advanced");
                cJSON_AddStringToObject(request_json, "search_depth", "advanced");
                cJSON_AddNumberToObject(request_json, "max_results", 10);

                char* request_body = cJSON_PrintUnformatted(request_json);
                std::string body_str(request_body);
                cJSON_free(request_body);
                cJSON_Delete(request_json);

                // Use Tavily Search API
                std::string url = "https://api.tavily.com/search";

                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                http->SetHeader("Authorization", "Bearer " + api_key);
                http->SetHeader("Content-Type", "application/json");
                http->SetContent(std::move(body_str));

                if (!http->Open("POST", url)) {
                    ESP_LOGE(TAG, "Failed to open HTTP connection for search");
                    return std::string("Failed to connect to search service");
                }

                int status_code = http->GetStatusCode();
                if (status_code != 200) {
                    ESP_LOGE(TAG, "Search request failed with status: %d", status_code);
                    http->Close();
                    return std::string("Search request failed with status " + std::to_string(status_code));
                }

                ESP_LOGI(TAG, "Response length: %d",   http->GetBodyLength());

                std::string response;
                char buffer[1024];
                int len;
                // Use Read() loop to avoid 8KB limit in ReadAll() which causes deadlock on large responses
                while ((len = http->Read(buffer, sizeof(buffer))) > 0) {
                    response.append(buffer, len);
                }

                http->Close();

                // Parse the Tavily response
                cJSON* json = cJSON_Parse(response.c_str());
                if (json == nullptr) {
                    ESP_LOGE(TAG, "Failed to parse search response");
                    return std::string("Failed to parse search response");
                }

                // Extract answer
                cJSON* answer = cJSON_GetObjectItem(json, "answer");
                std::string answer_str;
                if (cJSON_IsString(answer) && strlen(answer->valuestring) > 0) {
                    answer_str = answer->valuestring;
                } else {
                    answer_str = "No answer found";
                }

                cJSON_Delete(json);

                ESP_LOGI(TAG, "Search answer: %s", answer_str.c_str());

                led->SetBrightness(SPEAKING_BRIGHTNESS);
                led->SetColor(255, 165, 0); // Orange
                led->TurnOn();

                return answer_str;
            });

        mcp_server.AddTool("self.curtains.open",
            "Open the curtains.",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                const std::string url = "http://192.168.3.80/cover/curtain/open";
                ESP_LOGI(TAG, "Sending curtain open command");

                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                if (!http->Open("POST", url)) {
                    ESP_LOGE(TAG, "Failed to open HTTP connection for curtain open");
                    return std::string("Failed to connect to curtain controller");
                }

                const int status_code = http->GetStatusCode();
                http->Close();
                if (status_code < 200 || status_code >= 300) {
                    ESP_LOGE(TAG, "Curtain open command failed with status: %d", status_code);
                    return std::string("Curtain open command failed with status " + std::to_string(status_code));
                }

                return std::string("Curtain opening command sent");
            });

        mcp_server.AddTool("self.curtains.close",
            "Close the curtains.",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                const std::string url = "http://192.168.3.80/cover/curtain/close";
                ESP_LOGI(TAG, "Sending curtain close command");

                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                if (!http->Open("POST", url)) {
                    ESP_LOGE(TAG, "Failed to open HTTP connection for curtain close");
                    return std::string("Failed to connect to curtain controller");
                }

                const int status_code = http->GetStatusCode();
                http->Close();
                if (status_code < 200 || status_code >= 300) {
                    ESP_LOGE(TAG, "Curtain close command failed with status: %d", status_code);
                    return std::string("Curtain close command failed with status " + std::to_string(status_code));
                }

                return std::string("Curtain close command sent");
            });

        // iLink BLE lamp control
        ilink_lamp_controller_ = new ILinkLampController();
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
    }

    static void IdleTimerCallback(void* arg) {
        ((FluffyBoard*)arg)->TurnOff();

        esp_rom_delay_us(500000);  // Busy wait for 500ms

        //if we are still here - we are on craddle and can not turm off
        //restore keep on pin - toy sould no turn of when user takes it from craddle
        gpio_set_level(KEEP_ON_PIN, 1);  
        ESP_LOGI(TAG, "Surwived turnoff, restoring KEEP_ON_PIN high");

        ((FluffyBoard*)arg)->RestartIdleTimer();
    }

    esp_err_t ReadBatteryAdcOnce(int* adc_raw) {
        if (shared_adc_handle_ == nullptr) {
            return ESP_ERR_INVALID_STATE;
        }
        return adc_oneshot_read(shared_adc_handle_, kBatteryAdcChannel, adc_raw);
    }

    esp_err_t ReadBatteryRawAndGpioVoltageMvOnce(int* adc_raw, int* gpio_mv) {
        esp_err_t err = ReadBatteryAdcOnce(adc_raw);
        if (err != ESP_OK) {
            return err;
        }

        if (adc_calibration_enabled_ && adc_cali_handle_ != nullptr) {
            err = adc_cali_raw_to_voltage(adc_cali_handle_, *adc_raw, gpio_mv);
            if (err == ESP_OK) {
                return ESP_OK;
            }
            ESP_LOGW(TAG, "ADC calibration convert failed, fallback to approximate conversion: %s", esp_err_to_name(err));
        }

        // Approximate fallback if calibration is unavailable.
        *gpio_mv = (*adc_raw * 3300) / 4095;
        return ESP_OK;
    }

    void SampleBatteryGpio6() {
        std::array<int, kAdcBurstSamples> raw_samples{};
        std::array<int, kAdcBurstSamples> mv_samples{};
        size_t count = 0;

        for (size_t i = 0; i < kAdcBurstSamples; ++i) {
            int adc_raw = 0;
            int gpio_mv = 0;
            esp_err_t err = ReadBatteryRawAndGpioVoltageMvOnce(&adc_raw, &gpio_mv);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to read GPIO6 ADC sample %u: %s",
                         static_cast<unsigned>(i), esp_err_to_name(err));
                continue;
            }
            raw_samples[count] = adc_raw;
            mv_samples[count] = gpio_mv;
            count++;
        }

        if (count == 0) {
            ESP_LOGW(TAG, "GPIO6 ADC read failed for all samples");
            return;
        }

        std::sort(raw_samples.begin(), raw_samples.begin() + count);
        std::sort(mv_samples.begin(), mv_samples.begin() + count);

        size_t start = 0;
        size_t end = count;
        if (count > (kAdcTrimEachSide * 2)) {
            start = kAdcTrimEachSide;
            end = count - kAdcTrimEachSide;
        }

        int64_t raw_sum = 0;
        int64_t mv_sum = 0;
        for (size_t i = start; i < end; ++i) {
            raw_sum += raw_samples[i];
            mv_sum += mv_samples[i];
        }

        size_t used = end - start;
        int raw_avg = static_cast<int>(raw_sum / static_cast<int64_t>(used));
        int mv_avg = static_cast<int>(mv_sum / static_cast<int64_t>(used));

        // Do not let pressed-button samples affect battery estimation.
        if (mv_avg > static_cast<int>(kTouchPressedMaxMv)) {
            if (battery_sample_count_ < kBatteryAverageSamples) {
                battery_samples_[battery_sample_count_] = mv_avg;
                battery_sample_sum_ += mv_avg;
                battery_sample_count_++;
            } else {
                battery_sample_sum_ -= battery_samples_[battery_sample_index_];
                battery_samples_[battery_sample_index_] = mv_avg;
                battery_sample_sum_ += mv_avg;
                battery_sample_index_ = (battery_sample_index_ + 1) % kBatteryAverageSamples;
            }
        } else {
            ESP_LOGI(TAG, "GPIO6 adc_raw=%d gpio_mv=%d (button pressed, skip battery sample)", raw_avg, mv_avg);
        }

        if (battery_sample_count_ == 0) {
            ESP_LOGI(TAG, "GPIO6 adc_raw=%d battery=pending", raw_avg);
            return;
        }

        float mv_batt_avg = static_cast<float>(battery_sample_sum_) / static_cast<float>(battery_sample_count_);
        float battery_voltage = (mv_batt_avg / 1000.0f) * kBatteryVoltageScale;
        ESP_LOGI(TAG, "GPIO6 adc_raw=%d battery=%.3fV", raw_avg, battery_voltage);
    }

    static void BatteryTimerCallback(void* arg) {
        ((FluffyBoard*)arg)->SampleBatteryGpio6();
    }

public:
    FluffyBoard() :
        boot_button_(BOOT_BUTTON_GPIO) {

        gpio_set_direction(KEEP_ON_PIN, GPIO_MODE_OUTPUT);
        gpio_set_level(KEEP_ON_PIN, 1);

        InitializeSharedAdc();
        InitializeAdcCalibration();
        InitializeButtons();
        InitializeBatteryMonitor();

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
        audio_codec.SetInputGain(20.0);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        static Display* display_ = new NoDisplay();
        return display_;
    }

    ~FluffyBoard() {
        if (touch_button_ != nullptr) {
            delete touch_button_;
            touch_button_ = nullptr;
        }
        if (battery_timer_ != nullptr) {
            esp_timer_stop(battery_timer_);
            esp_timer_delete(battery_timer_);
        }
        if (shared_adc_handle_ != nullptr) {
            adc_oneshot_del_unit(shared_adc_handle_);
            shared_adc_handle_ = nullptr;
        }
        if (adc_cali_handle_ != nullptr) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
            adc_cali_delete_scheme_curve_fitting(adc_cali_handle_);
#endif
            adc_cali_handle_ = nullptr;
        }
        if (idle_timer_ != nullptr) {
            esp_timer_stop(idle_timer_);
            esp_timer_delete(idle_timer_);
        }
        delete ilink_lamp_controller_;
    }
};

DECLARE_BOARD(FluffyBoard);
