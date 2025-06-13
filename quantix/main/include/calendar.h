#ifndef CALENDAR_H
#define CALENDAR_H
#include "freertos/event_groups.h"

extern TaskHandle_t xCalendarStartupHandle;
extern TaskHandle_t xPrefetchCalendarTaskHandle;
extern TaskHandle_t xCalendarStartupNoWifiHandle; // Handle for CalenderStartupNoWifi task
extern EventGroupHandle_t sleep_event_group;

// Directory for calendar data in LittleFS
#define CALENDAR_DIR "/littlefs/calendar"

void calendar_startup(void *pvParameters);
void CalenderStartupNoWifi(void *pvParameters);// In calendar.h (or extern declarations in calendar.c if no .h)

void deep_sleep_manager_task(void *pvParameters);

void prefetch_calendar_task(void *pvParameters);

#endif // CALENDAR_H