#include "gpio_rgb_led.h"
#include "application.h"
#include "device_state.h"
#include <esp_log.h>

#define TAG "GpioRGBLed"

#define DEFAULT_BRIGHTNESS 50
#define HIGH_BRIGHTNESS 100
#define LOW_BRIGHTNESS 10

#define IDLE_BRIGHTNESS 30
#define SPEAKING_BRIGHTNESS 100
#define UPGRADING_BRIGHTNESS 25
#define ACTIVATING_BRIGHTNESS 35

#define BLINK_INFINITE -1

// GPIO_RGB_LED
#define LEDC_LS_TIMER          LEDC_TIMER_1
#define LEDC_LS_MODE           LEDC_LOW_SPEED_MODE
#define LEDC_LS_CH0_CHANNEL    LEDC_CHANNEL_0
#define LEDC_LS_CH1_CHANNEL    LEDC_CHANNEL_1
#define LEDC_LS_CH2_CHANNEL    LEDC_CHANNEL_2

#define LEDC_DUTY              (8191)
// GPIO_RGB_LED

// Color definitions (RGB 0-255)
#define COLOR_RED    {255, 0, 0}
#define COLOR_GREEN  {0, 255, 0}
#define COLOR_BLUE   {0, 0, 255}
#define COLOR_YELLOW {255, 255, 0}
#define COLOR_CYAN   {0, 255, 255}
#define COLOR_MAGENTA {255, 0, 255}
#define COLOR_WHITE  {255, 255, 255}
#define COLOR_OFF    {0, 0, 0}

GpioRGBLed::GpioRGBLed(gpio_num_t gpio_r, gpio_num_t gpio_g, gpio_num_t gpio_b)
        : GpioRGBLed(gpio_r, gpio_g, gpio_b, 0, 0, 0,
                     LEDC_LS_TIMER, LEDC_LS_CH0_CHANNEL, LEDC_LS_CH1_CHANNEL, LEDC_LS_CH2_CHANNEL) {
}

GpioRGBLed::GpioRGBLed(gpio_num_t gpio_r, gpio_num_t gpio_g, gpio_num_t gpio_b,
                       int invert_r, int invert_g, int invert_b)
        : GpioRGBLed(gpio_r, gpio_g, gpio_b, invert_r, invert_g, invert_b,
                     LEDC_LS_TIMER, LEDC_LS_CH0_CHANNEL, LEDC_LS_CH1_CHANNEL, LEDC_LS_CH2_CHANNEL) {
}

GpioRGBLed::GpioRGBLed(gpio_num_t gpio_r, gpio_num_t gpio_g, gpio_num_t gpio_b,
                       int invert_r, int invert_g, int invert_b,
                       ledc_timer_t timer_num, ledc_channel_t channel_r,
                       ledc_channel_t channel_g, ledc_channel_t channel_b) {
    // If any gpio is not connected, you should use NoLed class
    assert(gpio_r != GPIO_NUM_NC);
    assert(gpio_g != GPIO_NUM_NC);
    assert(gpio_b != GPIO_NUM_NC);

    /*
     * Prepare and set configuration of timers
     * that will be used by LED Controller
     */
    ledc_timer_config_t ledc_timer = {};
    ledc_timer.duty_resolution = LEDC_TIMER_13_BIT;  // resolution of PWM duty
    ledc_timer.freq_hz = 4000;                      // frequency of PWM signal
    ledc_timer.speed_mode = LEDC_LS_MODE;           // timer mode
    ledc_timer.timer_num = timer_num;               // timer index
    ledc_timer.clk_cfg = LEDC_AUTO_CLK;              // Auto select the source clock

    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Initialize all three channels
    InitializeLedcChannel(gpio_r, invert_r, timer_num, channel_r, ledc_channel_r_);
    InitializeLedcChannel(gpio_g, invert_g, timer_num, channel_g, ledc_channel_g_);
    InitializeLedcChannel(gpio_b, invert_b, timer_num, channel_b, ledc_channel_b_);

    esp_timer_create_args_t blink_timer_args = {
        .callback = [](void *arg) {
            auto led = static_cast<GpioRGBLed*>(arg);
            led->OnBlinkTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "RGB Blink Timer",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&blink_timer_args, &blink_timer_));

    ledc_initialized_ = true;
}

GpioRGBLed::~GpioRGBLed() {
    esp_timer_stop(blink_timer_);
}

void GpioRGBLed::InitializeLedcChannel(gpio_num_t gpio, int output_invert,
                                        ledc_timer_t timer_num, ledc_channel_t channel,
                                        ledc_channel_config_t& ledc_channel) {
    ledc_channel.channel    = channel;
    ledc_channel.duty       = 0;
    ledc_channel.gpio_num   = gpio;
    ledc_channel.speed_mode = LEDC_LS_MODE;
    ledc_channel.hpoint     = 0;
    ledc_channel.timer_sel  = timer_num;
    ledc_channel.flags.output_invert = output_invert & 0x01;

    // Set LED Controller with previously prepared configuration
    ledc_channel_config(&ledc_channel);
}

void GpioRGBLed::UpdateDutyCycle() {
    if (!ledc_initialized_) {
        return;
    }

    // Calculate duty cycles with brightness scaling
    // brightness_ is 0-100, RGB values are 0-255
    uint32_t duty_r = (red_ * brightness_ / 100 * LEDC_DUTY) / 255;
    uint32_t duty_g = (green_ * brightness_ / 100 * LEDC_DUTY) / 255;
    uint32_t duty_b = (blue_ * brightness_ / 100 * LEDC_DUTY) / 255;

    ledc_set_duty(ledc_channel_r_.speed_mode, ledc_channel_r_.channel, duty_r);
    ledc_set_duty(ledc_channel_g_.speed_mode, ledc_channel_g_.channel, duty_g);
    ledc_set_duty(ledc_channel_b_.speed_mode, ledc_channel_b_.channel, duty_b);

    ledc_update_duty(ledc_channel_r_.speed_mode, ledc_channel_r_.channel);
    ledc_update_duty(ledc_channel_g_.speed_mode, ledc_channel_g_.channel);
    ledc_update_duty(ledc_channel_b_.speed_mode, ledc_channel_b_.channel);
}

void GpioRGBLed::SetColor(uint8_t red, uint8_t green, uint8_t blue) {
    red_ = red;
    green_ = green;
    blue_ = blue;
    UpdateDutyCycle();
}

void GpioRGBLed::SetBrightness(uint8_t brightness) {
    brightness_ = brightness;
    UpdateDutyCycle();
}

void GpioRGBLed::TurnOn() {
    if (!ledc_initialized_) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(blink_timer_);
    UpdateDutyCycle();
}

void GpioRGBLed::TurnOff() {
    if (!ledc_initialized_) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(blink_timer_);
    
    ledc_set_duty(ledc_channel_r_.speed_mode, ledc_channel_r_.channel, 0);
    ledc_set_duty(ledc_channel_g_.speed_mode, ledc_channel_g_.channel, 0);
    ledc_set_duty(ledc_channel_b_.speed_mode, ledc_channel_b_.channel, 0);
    
    ledc_update_duty(ledc_channel_r_.speed_mode, ledc_channel_r_.channel);
    ledc_update_duty(ledc_channel_g_.speed_mode, ledc_channel_g_.channel);
    ledc_update_duty(ledc_channel_b_.speed_mode, ledc_channel_b_.channel);
}

void GpioRGBLed::BlinkOnce() {
    Blink(1, 100);
}

void GpioRGBLed::Blink(int times, int interval_ms) {
    StartBlinkTask(times, interval_ms);
}

void GpioRGBLed::StartContinuousBlink(int interval_ms) {
    StartBlinkTask(BLINK_INFINITE, interval_ms);
}

void GpioRGBLed::StartBlinkTask(int times, int interval_ms) {
    if (!ledc_initialized_) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    esp_timer_stop(blink_timer_);

    blink_counter_ = times * 2;
    blink_interval_ms_ = interval_ms;
    esp_timer_start_periodic(blink_timer_, interval_ms * 1000);
}

void GpioRGBLed::OnBlinkTimer() {
    std::lock_guard<std::mutex> lock(mutex_);
    blink_counter_--;
    if (blink_counter_ & 1) {
        // Turn on with current color
        UpdateDutyCycle();
    } else {
        // Turn off
        ledc_set_duty(ledc_channel_r_.speed_mode, ledc_channel_r_.channel, 0);
        ledc_set_duty(ledc_channel_g_.speed_mode, ledc_channel_g_.channel, 0);
        ledc_set_duty(ledc_channel_b_.speed_mode, ledc_channel_b_.channel, 0);
        
        ledc_update_duty(ledc_channel_r_.speed_mode, ledc_channel_r_.channel);
        ledc_update_duty(ledc_channel_g_.speed_mode, ledc_channel_g_.channel);
        ledc_update_duty(ledc_channel_b_.speed_mode, ledc_channel_b_.channel);

        if (blink_counter_ == 0) {
            esp_timer_stop(blink_timer_);
        }
    }
}

void GpioRGBLed::OnStateChanged() {
    auto& app = Application::GetInstance();
    auto device_state = app.GetDeviceState();
    
    switch (device_state) {
        case kDeviceStateStarting:
            SetBrightness(DEFAULT_BRIGHTNESS);
            SetColor(255, 255, 0); // Yellow
            StartContinuousBlink(100);
            break;
        case kDeviceStateWifiConfiguring:
            SetBrightness(DEFAULT_BRIGHTNESS);
            SetColor(255, 165, 0); // Orange
            StartContinuousBlink(500);
            break;
        case kDeviceStateIdle:
            SetBrightness(IDLE_BRIGHTNESS);
            SetColor(0, 0, 255); // Blue
            TurnOn();
            break;
        case kDeviceStateConnecting:
            SetBrightness(DEFAULT_BRIGHTNESS);
            SetColor(0, 255, 255); // Cyan
            TurnOn();
            break;
        case kDeviceStateListening:
        case kDeviceStateAudioTesting:
            if (app.IsVoiceDetected()) {
                SetBrightness(HIGH_BRIGHTNESS);
                SetColor(0, 255, 0); // Green
            } else {
                SetBrightness(DEFAULT_BRIGHTNESS);
                SetColor(0, 255, 0); // Green
            }
            TurnOn();
            break;
        case kDeviceStateSpeaking:
            SetBrightness(SPEAKING_BRIGHTNESS);
            SetColor(255, 165, 0); // Orange
            TurnOn();
            break;
        case kDeviceStateUpgrading:
            SetBrightness(UPGRADING_BRIGHTNESS);
            SetColor(255, 255, 0); // Yellow
            StartContinuousBlink(100);
            break;
        case kDeviceStateActivating:
            SetBrightness(ACTIVATING_BRIGHTNESS);
            SetColor(255, 165, 0); // Orange
            StartContinuousBlink(500);
            break;
        default:
            ESP_LOGE(TAG, "Unknown gpio rgb led event: %d", device_state);
            return;
    }
}
