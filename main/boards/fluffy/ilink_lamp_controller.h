#ifndef __ILINK_LAMP_CONTROLLER_H__
#define __ILINK_LAMP_CONTROLLER_H__

#include "mcp_server.h"
#include "iLink.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>

#define ILINK_TAG "ILinkLamp"

class ILinkLampController {
private:
    bool initialized_ = false;
    bool has_lamp_ = false;
    bool stop_task_ = false;
    esp_bd_addr_t cached_bda_ = {0};
    TaskHandle_t discovery_task_ = nullptr;
    SemaphoreHandle_t ble_mutex_ = nullptr;

    bool EnsureInitialized();
    bool ScanAndCache();
    static void DiscoveryTask(void* arg);
    std::string RunCommand(std::function<esp_err_t(ILink&)> cmd);

public:
    ILinkLampController();
    ~ILinkLampController();
};

#endif // __ILINK_LAMP_CONTROLLER_H__
