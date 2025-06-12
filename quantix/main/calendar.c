#include "EC11_driver.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "font_task.h"
#include "net_task.h"
#include "nvs.h"
#include "ui_task.h"
#include <errno.h> // For errno, ENOENT
#include <freertos/FreeRTOS.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>   // For malloc, free
#include <string.h>   // For strlen
#include <sys/time.h> // For struct timeval, time_t
#include <time.h>

// 標籤，用於日誌輸出
#define TAG_CALENDAR "CALENDAR" // Changed to avoid conflict with other TAG defines
// 日曆 API 的 URL
#define CALENDAR_URL "https://peng-pc.tail941dce.ts.net/api/calendar"

// LittleFS 中日曆數據的目錄
#define CALENDAR_DIR "/littlefs/calendar"
// 用於存儲日曆事件相關 API 回應的靜態緩衝區

#define CALENDAR_NOTIFY_INDEX_CMD 0
#define CALENDAR_NOTIFY_INDEX_DATA_READY 1

static char calendar_api_response_buffer[512];
TaskHandle_t xCalendarStartupHandle;
TaskHandle_t xPlusOneDayHandle;

// 解析日期string (YYYY-MM-DD 或 YYYY-MM-DDTHH:MM:SS...) 到 struct tm 結構體
// 成功返回 true，失敗返回 false。
bool parse_date_string(const char *date_str, struct tm *tm_out) {
    if (!date_str || !tm_out)
        return false;

    memset(tm_out, 0, sizeof(struct tm));

    // Try parsing YYYY-MM-DDTHH:MM:SS...
    // strptime 用於將string轉換為時間結構
    char *parse_end = strptime(date_str, "%Y-%m-%dT%H:%M:%S", tm_out);
    if (parse_end && (*parse_end == '+' || *parse_end == '-' || *parse_end == 'Z')) {
        // 成功解析 YYYY-MM-DDTHH:MM:SS，如果需要，可以處理時區/Z (或忽略日期部分)
        // 僅對於日期部分，我們可以在這裡停止。
        return true;
    }

    // 如果不是 YYYY-MM-DDTHH:MM:SS...，嘗試解析 YYYY-MM-DD
    parse_end = strptime(date_str, "%Y-%m-%d", tm_out);
    if (parse_end && *parse_end == '\0') {
        // 成功解析 YYYY-MM-DD
        return true;
    }

    // 如果解析失敗，嘗試將 tm_isdst 設置為 -1 (未知)
    if (tm_out->tm_year == 0 && tm_out->tm_mon == 0 && tm_out->tm_mday == 0) {
        tm_out->tm_isdst = -1; // 讓 mktime 決定夏令時
    }

    ESP_LOGE(TAG_CALENDAR, "Failed to parse date string: %s", date_str);
    return false;
}
// Saves an event JSON object to a file named YYYY-MM-DD.json in CALENDAR_DIR
// The file will contain a JSON array of events.
// Appends the new event to the existing array or creates a new file with the event.
esp_err_t save_event_for_date(const char *date_yyyymmdd, const cJSON *event_json) {
    if (!date_yyyymmdd || !event_json) {
        return ESP_ERR_INVALID_ARG;
    }

    // 構造檔案路徑: CALENDAR_DIR/YYYY-MM-DD.json
    char file_path[64];
    snprintf(file_path, sizeof(file_path), "%s/%s.json", CALENDAR_DIR, date_yyyymmdd);

    cJSON *events_for_day = NULL;
    char *existing_content = NULL;
    FILE *f = fopen(file_path, "rb"); // 以二進位讀取模式打開檔案

    if (f) {
        // 檔案存在，讀取內容
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fsize > 0) {
            existing_content = malloc(fsize + 1);
            if (existing_content) {
                fread(existing_content, 1, fsize, f);
                existing_content[fsize] = '\0';
                events_for_day = cJSON_Parse(existing_content);
                if (!events_for_day || !cJSON_IsArray(events_for_day)) {
                    ESP_LOGW(TAG_CALENDAR,
                             "Existing file %s is not a valid JSON array, will overwrite.",
                             file_path);
                    cJSON_Delete(events_for_day);
                    events_for_day = cJSON_CreateArray();
                }
            } else {
                ESP_LOGE(TAG_CALENDAR, "Failed to allocate memory for reading %s", file_path);
                fclose(f);
                return ESP_ERR_NO_MEM;
            }
        } else {
            events_for_day = cJSON_CreateArray(); // 檔案為空
        }
        fclose(f);
        free(existing_content);
    } else {
        events_for_day = cJSON_CreateArray(); // 檔案不存在
    }

    if (!events_for_day) {
        ESP_LOGE(TAG_CALENDAR, "Failed to get or create JSON array for %s", file_path);
        return ESP_FAIL;
    }

    // 檢查陣列中是否已存在具有相同摘要的事件
    bool event_exists = false;
    cJSON *existing_event_iterator = NULL;
    cJSON *new_summary_item = cJSON_GetObjectItemCaseSensitive(event_json, "summary");

    cJSON_ArrayForEach(existing_event_iterator, events_for_day) {
        cJSON *existing_summary_item =
            cJSON_GetObjectItemCaseSensitive(existing_event_iterator, "summary");
        cJSON *existing_start_item =
            cJSON_GetObjectItemCaseSensitive(existing_event_iterator, "start");

        if (cJSON_IsString(existing_summary_item) && cJSON_IsString(existing_start_item) &&
            cJSON_IsString(new_summary_item) && existing_summary_item->valuestring &&
            existing_start_item->valuestring && new_summary_item->valuestring &&
            (strcmp(existing_summary_item->valuestring, new_summary_item->valuestring) == 0) &&
            (strcmp(existing_start_item->valuestring, new_summary_item->valuestring) == 0)) {
            event_exists = true; // Event already exists
            ESP_LOGI(TAG_CALENDAR, "Event with summary '%s' already exists in %s, skipping.",
                     new_summary_item->valuestring, file_path);
            break;
        }
    }

    if (!event_exists) {
        cJSON *event_copy = cJSON_Duplicate(event_json, true);
        if (!event_copy) {
            ESP_LOGE(TAG_CALENDAR, "Failed to duplicate event JSON for saving.");
            cJSON_Delete(events_for_day); // 返回前清理 events_for_day
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(events_for_day, event_copy);
    }

    char *updated_content = cJSON_PrintUnformatted(events_for_day);
    // events_for_day 現在由 updated_content 表示或已被清理
    cJSON_Delete(events_for_day);

    if (!updated_content) {
        ESP_LOGE(TAG_CALENDAR, "Failed to print updated JSON array.");
        return ESP_FAIL;
    }

    // 將更新的內容寫回檔案 (覆蓋)
    f = fopen(file_path, "wb"); // 使用 "wb" 覆蓋
    if (!f) {
        ESP_LOGE(TAG_CALENDAR, "Failed to open file for writing: %s", file_path);
        free(updated_content);
        return ESP_FAIL;
    }

    size_t written = fwrite(updated_content, 1, strlen(updated_content), f);
    fclose(f);

    if (written != strlen(updated_content)) {
        // 注意：此處的 updated_content 已經被 free，日誌中不應再使用
        ESP_LOGE(
            TAG_CALENDAR, "Failed to write full content to %s (wrote %zu / expected %zu)",
            file_path, written,
            strlen(updated_content) /* 此處 strlen(updated_content) 會有問題，因為它已被釋放 */);
        // 應在 free 之前記錄 updated_content 的長度
        return ESP_FAIL;
    }
    free(updated_content); // 釋放 cJSON_PrintUnformatted 分配的記憶體

    if (!event_exists) {
        ESP_LOGI(TAG_CALENDAR, "New event saved to %s", file_path);
    } else {
        // ESP_LOGI(TAG, "檔案 %s 已更新 (或事件摘要已存在)。", file_path);
    }
    return ESP_OK;
}

// 檢查日曆設定是否存在於 NVS 中
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

// 收集日曆事件數據的回調函數，在網路請求完成後調用
void collect_event_data_callback(net_event_t *event, esp_err_t result) {
    char date[11] = {0};
    if (result == ESP_OK && event->json_root) {
        // event_t ev_cal;
        // ev_cal.event_id = SCREEN_EVENT_CALENDAR;
        if (event->user_data) {
            strncpy(date, (char *)event->user_data, sizeof(date) - 1);
            // ev_cal.msg[sizeof(ev_cal.msg) - 1] = '\0';
            // ESP_LOGI(TAG_CALENDAR, "Sending SCREEN_EVENT_CALENDAR event for date: %s",
            // ev_cal.msg);
        } else {
            ESP_LOGW(TAG_CALENDAR, "user_data (date) for calendar UI event is NULL.");
            // snprintf(ev_cal.msg, sizeof(ev_cal.msg), "NoDate");
        }
        // 注意：我們在釋放 user_data 之前發送 UI 事件，
        // 因為queue發送會複製 event_t 結構。

        // 如果存在，釋放動態分配的 post_data
        // 此回調知道對於此特定事件類型，post_data 是 malloc 分配的。
        if (event->post_data) {
            free((void *)event->post_data);
            // 此處將 event->post_data 設置為 NULL 並非嚴格必要，因為事件
            // 結構是從queue複製的副本，並將在此回調後被丟棄，
            // 但如果結構具有更長的生命週期，則這是一個好習慣。
            // event->post_data = NULL;
        }
        // 釋放動態分配的 user_data (日期string)
        if (event->user_data) {
            free(event->user_data);
            // event->user_data = NULL; // 與 post_data 原因相同，並非嚴格必要
        }

        ESP_LOGI(TAG_CALENDAR, "Received calendar data, processing events...");
        cJSON *events_array = cJSON_GetObjectItemCaseSensitive(event->json_root, "events");

        if (cJSON_IsArray(events_array)) {
            char missing_chars_total[256] = {0};
            size_t current_missing_len = 0;
            // 最大缺失字元緩衝區長度，減去 HEX_KEY_LEN-1 (字元本身的 hex 長度) 和 1 (null 終止符)
            const size_t max_missing_len = sizeof(missing_chars_total) - (HEX_KEY_LEN - 1) - 1;

#define MAX_UNIQUE_DATES_PER_FETCH 100 // 每次獲取最大唯一 YYYY-MM-DD string數量
            cJSON *event_scanner_json = NULL;

            char file_to_delete_path[64];
            snprintf(file_to_delete_path, sizeof(file_to_delete_path), "%s/%s.json", CALENDAR_DIR,
                     date);
            if (remove(file_to_delete_path) == 0) {
                ESP_LOGI(TAG_CALENDAR, "Cleared existing event file: %s", file_to_delete_path);
            } else {
                // ENOENT (No such file or directory) 是正常的，表示檔案本來就不存在
                if (errno != ENOENT) {
                    ESP_LOGW(TAG_CALENDAR, "Failed to delete %s: %s (errno %d)",
                             file_to_delete_path, strerror(errno), errno);
                }
            }
            errno = 0; // Reset errno

            // 第一遍：收集缺失的字元和將受影響的唯一日期
            cJSON_ArrayForEach(event_scanner_json, events_array) {
                if (!cJSON_IsObject(event_scanner_json)) {
                    ESP_LOGW(TAG_CALENDAR, "Skipping non-object item in events array (scan pass).");
                    continue;
                }

                // 1. 處理摘要以查找缺失字元
                cJSON *summary_item =
                    cJSON_GetObjectItemCaseSensitive(event_scanner_json, "summary");
                if (cJSON_IsString(summary_item) && (summary_item->valuestring != NULL)) {
                    const char *summary_utf8 = summary_item->valuestring;
                    char missing_for_event[256] = {0};
                    if (find_missing_characters(summary_utf8, missing_for_event,
                                                sizeof(missing_for_event)) > 0) {
                        if (current_missing_len + strlen(missing_for_event) <= max_missing_len) {
                            strcat(missing_chars_total, missing_for_event);
                            current_missing_len += strlen(missing_for_event);
                        } else {
                            // 如果缺少的字型太多，就先下載
                            download_missing_characters(missing_chars_total);
                            ESP_LOGW(TAG_CALENDAR,
                                     "Total missing characters buffer full. Cannot add more from "
                                     "event '%s'.",
                                     summary_utf8);
                            memset(missing_chars_total, 0, sizeof(missing_chars_total));
                            current_missing_len = strlen(missing_for_event);
                            strcat(missing_chars_total, missing_for_event);
                        }
                    }
                }
                download_missing_characters(missing_chars_total);

                cJSON *event_to_save = cJSON_Duplicate(event_scanner_json, true);
                if (!event_to_save) {
                    ESP_LOGE(TAG_CALENDAR, "Failed to duplicate event_json for saving.");
                    continue;
                }
                save_event_for_date(date, event_to_save);
                cJSON_Delete(event_to_save); // 清理複製的事件
            }
            // 處理完所有事件後，如果存在缺失字元，則下載它們
            ESP_LOGI(TAG_CALENDAR,
                     "Finished processing and saving calendar events. Notifying on index %d.",
                     CALENDAR_NOTIFY_INDEX_DATA_READY);
            xTaskNotifyGiveIndexed(xCalendarStartupHandle, CALENDAR_NOTIFY_INDEX_DATA_READY);
            // xQueueSend(gui_queue, &ev_cal, portMAX_DELAY); // 發送準備好的 UI 事件
        } else {
            ESP_LOGW(TAG_CALENDAR, "Events array is invalid or empty.");
        }
        cJSON_Delete(event->json_root); // 釋放解析的 JSON 樹
        event->json_root = NULL;        // 標記為已處理
    } else {
        // 即使失敗，也釋放動態分配的 post_data
        if (event->post_data) {
            free((void *)event->post_data);
            event->post_data = NULL;
        }
        // 即使失敗，也釋放動態分配的 user_data
        if (event->user_data) {
            free(event->user_data);
            event->user_data = NULL;
        }
        ESP_LOGE(TAG_CALENDAR,
                 "Failed to receive calendar data or JSON parse error. HTTP result: %s",
                 esp_err_to_name(result));
        if (event->response_buffer && strlen(event->response_buffer) > 0) {
            ESP_LOGE(TAG_CALENDAR, "Response: %s", event->response_buffer);
        }
        // 如果 json_root 不為 NULL (例如，解析在 net_task 中完成，但回調中檢測到其他錯誤)
        if (event->json_root) {
            cJSON_Delete(event->json_root);
            event->json_root = NULL;
        }
    }
}
// 根據指定時間收集事件數據
void collect_event_data(struct tm timeinfo) {
    char date[11];
    strftime(date, sizeof(date), "%Y-%m-%d", &timeinfo);

    // Dynamically allocate buffer for post_data
    // 為 post_data 動態分配緩衝區，{"date":"YYYY-MM-DD"} 大約需要 27 字節
    char *dynamic_post_data = malloc(27);
    if (!dynamic_post_data) {
        ESP_LOGE(TAG_CALENDAR, "Failed to allocate memory for post_data");
        return;
    }
    snprintf(dynamic_post_data, 27, "{\"date\":\"%s\"}", date);

    // 為 UI 事件消息複製日期string
    char *date_for_ui_event = strdup(date); // strdup 使用 malloc
    if (!date_for_ui_event) {
        ESP_LOGE(TAG_CALENDAR, "Failed to allocate memory for date_for_ui_event");
        free(dynamic_post_data); // 清理先前分配的記憶體
        return;
    }

    // 使用前清除靜態回應緩衝區
    calendar_api_response_buffer[0] = '\0';

    net_event_t event = {
        .url = CALENDAR_URL,
        .method = HTTP_METHOD_POST,
        .post_data = dynamic_post_data, // 使用動態分配的緩衝區
        .use_jwt = true,
        .save_to_buffer = true,
        .response_buffer = calendar_api_response_buffer, // 使用靜態日曆回應緩衝區
        .response_buffer_size = sizeof(calendar_api_response_buffer),
        .on_finish = collect_event_data_callback,
        .user_data = date_for_ui_event, // 將日期string傳遞給 UI
        .json_parse = 1,                // 請求 net_task 解析 JSON
    };
    xQueueSend(net_queue, &event, portMAX_DELAY);
}

// 日曆模塊啟動函數
void calendar_startup(void *pvParameters) {
    // 等待 WiFi 連接
    xEventGroupWaitBits(net_event_group, NET_WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
    // 設定 NTP 伺服器和操作模式
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    // 等待NTP同步完成
    ESP_LOGI(TAG_CALENDAR, "Waiting for NTP time synchronization...");
    time_t now;
    struct tm timeinfo;
    uint32_t time_shift = 0;
    // 初始化 timeinfo 避免在 sntp_get_sync_status() 返回 SNTP_SYNC_STATUS_RESET 時 localtime_r 出錯
    memset(&timeinfo, 0, sizeof(struct tm));
    localtime_r(&now, &timeinfo); // 獲取初始 (可能未同步的) 時間

    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET || timeinfo.tm_year < (2023 - 1900)) {
        vTaskDelay(200 / portTICK_PERIOD_MS); // 每2秒檢查一次
        time(&now);
        localtime_r(&now, &timeinfo);
        ESP_LOGI(TAG_CALENDAR, "Current time: %04d-%02d-%02d %02d:%02d:%02d, waiting for sync...",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour,
                 timeinfo.tm_min, timeinfo.tm_sec);
    }
    localtime_r(&now, &timeinfo);
    xEventGroupWaitBits(net_event_group, NET_SERVER_CONNECTED_BIT, false, true, portMAX_DELAY);
    if (check_calendar_settings() != ESP_OK) {
        event_t ev = {
            .event_id = SCREEN_EVENT_CENTER,
            .msg = "No calendar settings found. Generating QR code for calendar setup...",
        };
        xQueueSend(gui_queue, &ev, portMAX_DELAY);
        userSettings();
        ESP_LOGE(TAG_CALENDAR, "Failed to read calendar settings");
    } else {
        xEventGroupSetBits(net_event_group, NET_GOOGLE_TOKEN_AVAILABLE_BIT);
    }
    for (;;) {
        xEventGroupWaitBits(net_event_group, NET_GOOGLE_TOKEN_AVAILABLE_BIT, false, true,
                            portMAX_DELAY);
        time_shift = 0;
        // 等待命令通知 (來自EC11或網路任務的初始信號)
        ESP_LOGI(TAG_CALENDAR, "Waiting for command notification on index %d",
                 CALENDAR_NOTIFY_INDEX_CMD);
        xTaskNotifyWaitIndexed(CALENDAR_NOTIFY_INDEX_CMD, /* ulIndexToWaitOn */
                               pdTRUE,                    /* xClearCountOnEntry */
                               pdTRUE,                    /* xClearBitsOnExit */
                               &time_shift,               /* pulNotificationValue */
                               portMAX_DELAY);            /* xTicksToWait */
        switch (time_shift) {
        case 2:
            timeinfo.tm_mday += 1;
            break;
        case 1:
            timeinfo.tm_mday -= 1;
            break;
        default:
            break;
        }
        // 標準化 timeinfo (處理月份/年份的進位和借位)
        mktime(&timeinfo);

        ESP_LOGI(TAG_CALENDAR,
                 "Time shift processed, value: %" PRIu32
                 ". Current date for data collection: %04d-%02d-%02d",
                 time_shift, timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
        event_t ev = {
            .event_id = SCREEN_EVENT_CALENDAR,
        };
        strftime(ev.msg, 11, "%Y-%m-%d", &timeinfo);
        xQueueSend(gui_queue, &ev, portMAX_DELAY);
        collect_event_data(timeinfo);

        // 等待 collect_event_data 完成的通知
        ESP_LOGI(TAG_CALENDAR, "Waiting for data ready notification on index %d",
                 CALENDAR_NOTIFY_INDEX_DATA_READY);
        timeinfo.tm_mday += 1;
        mktime(&timeinfo); // 標準化
        ESP_LOGI(TAG_CALENDAR, "Prefetching for next day: %04d-%02d-%02d", timeinfo.tm_year + 1900,
                 timeinfo.tm_mon + 1, timeinfo.tm_mday);
        collect_event_data(timeinfo); // 預取下一天

        timeinfo.tm_mday -= 2;
        mktime(&timeinfo); // 標準化
        ESP_LOGI(TAG_CALENDAR, "Prefetching for previous day: %04d-%02d-%02d",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
        collect_event_data(timeinfo); // 預取前一天

        timeinfo.tm_mday += 1;
        mktime(&timeinfo); // 恢復到當前顯示的日期
        ESP_LOGW(TAG_CALENDAR, "Setting encoder callback to notify task %p on index %d",
                 xCalendarStartupHandle, CALENDAR_NOTIFY_INDEX_CMD);
        ec11_set_encoder_callback(xCalendarStartupHandle);
    }
}
