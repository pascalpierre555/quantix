#include "esp_log.h"
#include "esp_sntp.h"
#include "net_task.h"
#include "nvs.h"
#include <freertos/FreeRTOS.h>
#include <stdio.h>
#include <time.h>

#define TAG "CALENDAR"

char year[5] = {0};  // 用於存儲年份
char month[4] = {0}; // 用於存儲月份縮寫
char day[3] = {0};   // 用於存儲日期

void get_today_date_string(char *buf, size_t buf_size) {
    time_t now;
    struct tm timeinfo;
    int retry = 0;
    const int max_retries = 10;
    while (retry < max_retries) {
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year > (2023 - 1900)) {
            strftime(buf, buf_size, "%b %d, %Y", &timeinfo); // 例如 "Jun 03, 2025"
            strncpy(month, buf, 3);
            month[3] = '\0'; // 保證結尾
            strncpy(year, buf + 7, 4);
            year[4] = '\0'; // 保證結尾
            strncpy(day, buf + 4, 2);
            day[2] = '\0'; // 保證結尾
            return;
        }
        ESP_LOGI(TAG, "Waiting for time sync... (%d/%d)", retry, max_retries);
        vTaskDelay(pdMS_TO_TICKS(2000));
        retry++;
    }
    ESP_LOGW(TAG, "Failed to sync time");
}

void ntpStartup(void *PvParameters) {
    xEventGroupWaitBits(net_event_group, NET_WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);

    // 設定 NTP 伺服器
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    char date[13];
    get_today_date_string(date, sizeof(date));

    ESP_LOGI(TAG, "Today's date: %s", date);

    xEventGroupSetBits(net_event_group, NET_TIME_AVAILABLE_BIT);
    vTaskDelete(NULL);
}

void CalenderInit(void *pvParemeters) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs);
    if (err != ESP_OK)
        return err;
    size_t username_buf_size;
    err = nvs_get_str(nvs, "google_username", NULL, &username_buf_size);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }

    size_t password_buf_size;
}