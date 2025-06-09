#include "cJSON.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "font_task.h"
#include "net_task.h"
#include "nvs.h"
#include "ui_task.h"
#include <freertos/FreeRTOS.h>
#include <stdio.h>
#include <stdlib.h> // For malloc, free
#include <string.h> // For strlen
#include <time.h>

#define TAG "CALENDAR"
#define CALENDAR_URL "https://peng-pc.tail941dce.ts.net/api/calendar"

char year[5] = {0};  // 用於存儲年份
char month[4] = {0}; // 用於存儲月份縮寫
char day[3] = {0};   // 用於存儲日期

TaskHandle_t xcalendarStartupHandle = NULL;

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

esp_err_t check_calendar_settings(void) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("calendar", NVS_READONLY, &nvs);
    if (err != ESP_OK)
        return err;

    size_t buf_size;
    err = nvs_get_str(nvs, "email", NULL, &buf_size);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }
    return ESP_OK;
}

void calendarStartup_callback(net_event_t *event, esp_err_t result) {
    if (result == ESP_OK && event->json_root) {
        cJSON *events_array = cJSON_GetObjectItemCaseSensitive(event->json_root, "events");
        if (cJSON_IsArray(events_array) && cJSON_GetArraySize(events_array) > 0) {
            cJSON *first_event = cJSON_GetArrayItem(events_array, 0);
            if (cJSON_IsObject(first_event)) {
                cJSON *summary_item = cJSON_GetObjectItemCaseSensitive(first_event, "summary");
                if (cJSON_IsString(summary_item) && (summary_item->valuestring != NULL)) {
                    const char *summary_utf8 = summary_item->valuestring;
                    ESP_LOGI(TAG, "Original summary: %s", summary_utf8);
                    char missing[256] = {0};
                    find_missing_characters(summary_utf8, missing, sizeof(missing));
                    ESP_LOGI(TAG, "Missing characters: %s", missing);
                    download_missing_characters(missing);
                } else {
                    ESP_LOGW(TAG, "Summary item is not a string or is null.");
                }
            } else {
                ESP_LOGW(TAG, "First event is not an object.");
            }
        } else {
            ESP_LOGW(TAG, "Events array is not valid or empty.");
        }
    } else {
        ESP_LOGE(TAG, "Failed to parse JSON or HTTP error. Response: %s",
                 event->response_buffer ? event->response_buffer : "N/A");
    }
}

void calendarStartup(void *pvParameters) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // 等待通知喚醒
        if (check_calendar_settings() != ESP_OK) {
            event_t ev = {
                .event_id = SCREEN_EVENT_CENTER,
                .msg = "No calendar settings found. Generating QR code for calendar setup...",
            };
            xQueueSend(gui_queue, &ev, portMAX_DELAY);
            userSettings();
            ESP_LOGE(TAG, "Failed to read calendar settings");
        } else {
            ESP_LOGI(TAG, "Calendar settings found, proceeding with calendar setup");
            char post_data_buffer[27]; // 大小自己抓，不要爆掉
            char date[11];
            time_t now;
            struct tm timeinfo;
            time(&now);
            localtime_r(&now, &timeinfo);
            strftime(date, sizeof(date), "%Y-%m-%d", &timeinfo);
            snprintf(post_data_buffer, sizeof(post_data_buffer), "{\"date\":\"%s\"}", date);
            char responseBuffer[512] = {0}; // 確保有足夠的空間
            net_event_t event = {
                .url = CALENDAR_URL,
                .method = HTTP_METHOD_POST,
                .post_data = post_data_buffer,
                .use_jwt = true,
                .save_to_buffer = true,
                .response_buffer = responseBuffer,
                .response_buffer_size = sizeof(responseBuffer),
                .on_finish = calendarStartup_callback,
                .user_data = NULL,
                .json_root = (void *)1,
            };
            xQueueSend(net_queue, &event, portMAX_DELAY);
        }
    }
}

void ntpStartup(void *pvParameters) {
    xEventGroupWaitBits(net_event_group, NET_WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);

    // 設定 NTP 伺服器
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    xEventGroupSetBits(net_event_group, NET_TIME_AVAILABLE_BIT);
    vTaskDelete(NULL);
}
