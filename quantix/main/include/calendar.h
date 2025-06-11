#ifndef CALENDAR_H
#define CALENDAR_H

void ntpStartup(void *pvParameters);
void calendarStartup(void *pvParameters);

// Directory for calendar data in LittleFS
#define CALENDAR_DIR "/littlefs/calendar"

void calendar_startup(void);

#endif // CALENDAR_H