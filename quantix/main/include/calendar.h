#ifndef CALENDAR_H
#define CALENDAR_H
#include "freertos/event_groups.h"

extern TaskHandle_t xCalendarPrefetchHandle;
extern TaskHandle_t xPrefetchCalendarTaskHandle;
extern TaskHandle_t xCalendarDisplayHandle; // Handle for CalenderStartupNoWifi task
extern EventGroupHandle_t sleep_event_group;
extern RTC_DATA_ATTR bool isr_woken;

// Directory for calendar data in LittleFS
#define CALENDAR_DIR "/littlefs/calendar"

void calendar_prefetch_task(void *pvParameters);
void calendar_display(void *pvParameters);// In calendar.h (or extern declarations in calendar.c if no .h)

void deep_sleep_manager_task(void *pvParameters);

void calendar_prefetch_task(void *pvParameters);

#endif // CALENDAR_H