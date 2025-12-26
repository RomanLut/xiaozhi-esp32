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

#include <wifi_station.h>
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
