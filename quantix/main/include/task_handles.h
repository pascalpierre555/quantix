#ifndef TASK_HANDLES_H
#define TASK_HANDLES_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

extern TaskHandle_t xCalendarDisplayHandle;
extern TaskHandle_t xCalendarPrefetchHandle;

extern void screenStartup(void *pvParameters);
extern void calendar_display(void *pvParameters);
extern void calendarPrefetch(void *pvParameters);
extern void font_table_init(void);
extern EventGroupHandle_t net_event_group;

#define NET_WIFI_CONNECTED_BIT BIT0
#define NET_SERVER_CONNECTED_BIT BIT1
#define NET_TOKEN_AVAILABLE_BIT BIT2
#define NET_GOOGLE_TOKEN_AVAILABLE_BIT BIT3
#define NET_CALENDAR_AVAILABLE_BIT BIT4

#endif // TASK_HANDLES_H