#ifndef CALENDAR_H
#define CALENDAR_H

extern TaskHandle_t xCalendarStartupHandle;
extern TaskHandle_t xPrefetchCalendarTaskHandle;
extern TaskHandle_t xCalendarStartupNoWifiHandle; // Handle for CalenderStartupNoWifi task

// Directory for calendar data in LittleFS
#define CALENDAR_DIR "/littlefs/calendar"

void calendar_startup(void *pvParameters);
void CalenderStartupNoWifi(void *pvParameters);
void prefetch_calendar_task(void *pvParameters);

#endif // CALENDAR_H