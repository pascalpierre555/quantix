#ifndef UI_TASK_H
#define UI_TASK_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define EVENT_QUEUE_LENGTH 10
#define EVENT_QUEUE_ITEM_SIZE sizeof(event_t)
#define MAX_MSG_LEN 256

extern SemaphoreHandle_t xScreen;
extern TaskHandle_t xViewDisplayHandle;
extern QueueHandle_t gui_queue;
extern RTC_DATA_ATTR bool isr_woken;

// 定義螢幕顯示事件
enum {
    SCREEN_EVENT_WIFI_REQUIRED = 1,
    SCREEN_EVENT_NO_CONNECTION = 2,
    SCREEN_EVENT_CENTER = 3,
    SCREEN_EVENT_CLEAR = 4,
    SCREEN_EVENT_CALENDAR = 5,
    SCREEN_EVENT_QRCODE = 6,
};

typedef struct {
    int32_t event_id;
    char msg[MAX_MSG_LEN];
} event_t;

void setting_qrcode_setting(char *qrcode);
void screenStartup(void *pvParameters);
void viewDisplay(void *PvParameters);

#endif // UI_TASK_H