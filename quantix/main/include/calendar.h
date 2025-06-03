#ifndef CALENDAR_H
#define CALENDAR_H

void get_today_date_string(char *buf, size_t buf_size);
void ntpStartup(void *pvParameters);
void calendarInit(void);

extern char month[4]; // 用於存儲月份縮寫
extern char year[5];  // 用於存儲年份
extern char day[3];   // 用於存儲日期

#endif // CALENDAR_H