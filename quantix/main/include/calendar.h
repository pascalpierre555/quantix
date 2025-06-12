#ifndef CALENDAR_H
#define CALENDAR_H

extern TaskHandle_t xCalendarStartupHandle;

// Directory for calendar data in LittleFS
#define CALENDAR_DIR "/littlefs/calendar"

void calendar_startup(void *pvParameters);

#endif // CALENDAR_H