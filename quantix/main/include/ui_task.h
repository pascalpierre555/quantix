#ifndef UI_TASK_H
#define UI_TASK_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

extern SemaphoreHandle_t xScreen;
extern TaskHandle_t xViewDisplayHandle;

// 定義螢幕顯示事件
enum {
    SCREEN_EVENT_WIFI_REQUIRED = 1,
    SCREEN_EVENT_NO_CONNECTION = 2,
    SCREEN_EVENT_WIFI_CONNECTED = 3,
};

void screenStartup(void *pvParameters);
void viewDisplay(void *PvParameters);

#endif // UI_TASK_H