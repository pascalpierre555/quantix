#ifndef CALENDAR_H
#define CALENDAR_H

extern TaskHandle_t xCalendarStartupHandle;
extern TaskHandle_t xPrefetchCalendarTaskHandle;

// Directory for calendar data in LittleFS
#define CALENDAR_DIR "/littlefs/calendar"

void calendar_startup(void *pvParameters);
void prefetch_calendar_task(void *pvParameters);

#endif // CALENDAR_H