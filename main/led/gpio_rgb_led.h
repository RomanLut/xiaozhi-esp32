#ifndef _GPIO_RGB_LED_H_
#define _GPIO_RGB_LED_H_

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "led.h"
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>
#include <atomic>
#include <mutex>

class GpioRGBLed : public Led {
 public:
    // Constructor with 3 GPIO pins for Red, Green, Blue
    GpioRGBLed(gpio_num_t gpio_r, gpio_num_t gpio_g, gpio_num_t gpio_b);
    
    // Constructor with output inversion for each channel
    GpioRGBLed(gpio_num_t gpio_r, gpio_num_t gpio_g, gpio_num_t gpio_b,
               int invert_r, int invert_g, int invert_b);
    
    // Constructor with custom timer and channel configuration
    GpioRGBLed(gpio_num_t gpio_r, gpio_num_t gpio_g, gpio_num_t gpio_b,
               int invert_r, int invert_g, int invert_b,
               ledc_timer_t timer_num, ledc_channel_t channel_r,
               ledc_channel_t channel_g, ledc_channel_t channel_b);
    
    virtual ~GpioRGBLed();

    void OnStateChanged() override;
    
    // Set RGB color (0-255 for each component)
    void SetColor(uint8_t red, uint8_t green, uint8_t blue);
    
    // Turn on with current color
    void TurnOn();
    
    // Turn off (set all to 0)
    void TurnOff();
    
    // Set overall brightness (0-100) while maintaining color ratios
    void SetBrightness(uint8_t brightness);

 private:
    std::mutex mutex_;
    ledc_channel_config_t ledc_channel_r_ = {0};
    ledc_channel_config_t ledc_channel_g_ = {0};
    ledc_channel_config_t ledc_channel_b_ = {0};
    bool ledc_initialized_ = false;
    
    // Current RGB values (0-255)
    uint8_t red_ = 0;
    uint8_t green_ = 0;
    uint8_t blue_ = 0;
    
    // Overall brightness (0-100)
    uint8_t brightness_ = 100;
    
    // Blink control
    int blink_counter_ = 0;
    int blink_interval_ms_ = 0;
    esp_timer_handle_t blink_timer_ = nullptr;
    
    // Helper methods
    void InitializeLedcChannel(gpio_num_t gpio, int output_invert,
                                ledc_timer_t timer_num, ledc_channel_t channel,
                                ledc_channel_config_t& ledc_channel);
    void UpdateDutyCycle();
    
    void StartBlinkTask(int times, int interval_ms);
    void OnBlinkTimer();
    
    void BlinkOnce();
    void Blink(int times, int interval_ms);
    void StartContinuousBlink(int interval_ms);
};

#endif  // _GPIO_RGB_LED_H_
