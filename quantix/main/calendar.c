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

#define TAG "CALENDAR"
#define CALENDAR_URL "https://peng-pc.tail941dce.ts.net/api/calendar"

#define CALENDAR_DIR "/littlefs/calendar" // Directory for calendar data in LittleFS
char year[5] = {0};                       // 用於存儲年份
char month[4] = {0};                      // 用於存儲月份縮寫
char day[3] = {0};                        // 用於存儲日期

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

// Parses a date string (YYYY-MM-DD or YYYY-MM-DDTHH:MM:SS...) into a struct tm
// Returns true on success, false on failure.
bool parse_date_string(const char *date_str, struct tm *tm_out) {
    if (!date_str || !tm_out)
        return false;

    memset(tm_out, 0, sizeof(struct tm));

    // Try parsing YYYY-MM-DDTHH:MM:SS...
    char *parse_end = strptime(date_str, "%Y-%m-%dT%H:%M:%S", tm_out);
    if (parse_end && (*parse_end == '+' || *parse_end == '-' || *parse_end == 'Z')) {
        // Successfully parsed YYYY-MM-DDTHH:MM:SS, handle timezone/Z if needed (or ignore for date
        // part) For date part only, we can stop here.
        return true;
    }

    // If not YYYY-MM-DDTHH:MM:SS..., try parsing YYYY-MM-DD
    parse_end = strptime(date_str, "%Y-%m-%d", tm_out);
    if (parse_end && *parse_end == '\0') {
        // Successfully parsed YYYY-MM-DD
        return true;
    }

    // If parsing failed, try setting tm_isdst to -1 (unknown)
    if (tm_out->tm_year == 0 && tm_out->tm_mon == 0 && tm_out->tm_mday == 0) {
        tm_out->tm_isdst = -1; // Let mktime determine DST
    }

    ESP_LOGE(TAG, "Failed to parse date string: %s", date_str);
    return false;
}

// Saves an event JSON object to a file named YYYY-MM-DD.json in CALENDAR_DIR
// The file will contain a JSON array of events.
// Appends the new event to the existing array or creates a new file with the event.
esp_err_t save_event_for_date(const char *date_yyyymmdd, const cJSON *event_json) {
    if (!date_yyyymmdd || !event_json) {
        return ESP_ERR_INVALID_ARG;
    }

    char file_path[64]; // CALENDAR_DIR + "/" + YYYY-MM-DD.json + null
    snprintf(file_path, sizeof(file_path), "%s/%s.json", CALENDAR_DIR, date_yyyymmdd);

    cJSON *events_for_day = NULL;
    char *existing_content = NULL;
    FILE *f = fopen(file_path, "rb");

    if (f) {
        // File exists, read content
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
                    ESP_LOGW(TAG, "Existing file %s is not a valid JSON array, overwriting.",
                             file_path);
                    cJSON_Delete(events_for_day);
                    events_for_day = cJSON_CreateArray();
                }
            } else {
                ESP_LOGE(TAG, "Failed to allocate memory for reading %s", file_path);
                fclose(f);
                return ESP_ERR_NO_MEM;
            }
        } else {
            events_for_day = cJSON_CreateArray();
        } // File is empty
        fclose(f);
        free(existing_content);
    } else {
        events_for_day = cJSON_CreateArray();
    } // File does not exist

    if (!events_for_day) {
        ESP_LOGE(TAG, "Failed to get or create JSON array for %s", file_path);
        return ESP_FAIL;
    }

    // Check if an event with the same summary already exists in the array
    bool event_exists = false;
    cJSON *existing_event_iterator = NULL;
    cJSON *new_summary_item = cJSON_GetObjectItemCaseSensitive(event_json, "summary");

    cJSON_ArrayForEach(existing_event_iterator, events_for_day) {
        cJSON *existing_summary_item =
            cJSON_GetObjectItemCaseSensitive(existing_event_iterator, "summary");
        if (cJSON_IsString(existing_summary_item) && cJSON_IsString(new_summary_item) &&
            existing_summary_item->valuestring && new_summary_item->valuestring &&
            strcmp(existing_summary_item->valuestring, new_summary_item->valuestring) == 0) {
            event_exists = true;
            ESP_LOGI(TAG, "Event with summary '%s' already exists in %s, skipping.",
                     new_summary_item->valuestring, file_path);
            break;
        }
    }

    if (!event_exists) {
        cJSON *event_copy = cJSON_Duplicate(event_json, true);
        if (!event_copy) {
            ESP_LOGE(TAG, "Failed to duplicate event JSON for saving.");
            cJSON_Delete(events_for_day); // Clean up events_for_day before returning
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(events_for_day, event_copy);
    }

    char *updated_content = cJSON_PrintUnformatted(events_for_day);
    cJSON_Delete(
        events_for_day); // events_for_day is now represented by updated_content or was cleaned up

    if (!updated_content) {
        ESP_LOGE(TAG, "Failed to print updated JSON array.");
        return ESP_FAIL;
    }

    // Write the updated content back to the file (overwrite)
    f = fopen(file_path, "wb"); // Use "wb" to overwrite
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", file_path);
        free(updated_content);
        return ESP_FAIL;
    }

    size_t written = fwrite(updated_content, 1, strlen(updated_content), f);
    fclose(f);
    free(updated_content);

    if (written != strlen(updated_content)) {
        ESP_LOGE(TAG, "Failed to write complete content to %s", file_path);
        return ESP_FAIL;
    }

    if (!event_exists) {
        ESP_LOGI(TAG, "Saved new event to %s", file_path);
    } else {
        // ESP_LOGI(TAG, "File %s updated (or event summary was already present).", file_path);
    }
    return ESP_OK;
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
        ESP_LOGI(TAG, "Received calendar data, processing events...");
        cJSON *events_array = cJSON_GetObjectItemCaseSensitive(event->json_root, "events");

        if (cJSON_IsArray(events_array)) {
            char missing_chars_total[512] = {0};
            size_t current_missing_len = 0;
            const size_t max_missing_len = sizeof(missing_chars_total) - (HEX_KEY_LEN - 1) - 1;

#define MAX_UNIQUE_DATES_PER_FETCH 100                         // Max unique YYYY-MM-DD strings
            char unique_dates[MAX_UNIQUE_DATES_PER_FETCH][11]; // "YYYY-MM-DD\0"
            int unique_dates_count = 0;

            cJSON *event_scanner_json = NULL;
            // First pass: Collect missing characters and unique dates that will be affected
            cJSON_ArrayForEach(event_scanner_json, events_array) {
                if (!cJSON_IsObject(event_scanner_json)) {
                    ESP_LOGW(TAG, "Skipping non-object item in events array (scan pass).");
                    continue;
                }

                // 1. Process summary for missing characters
                cJSON *summary_item =
                    cJSON_GetObjectItemCaseSensitive(event_scanner_json, "summary");
                if (cJSON_IsString(summary_item) && (summary_item->valuestring != NULL)) {
                    const char *summary_utf8 = summary_item->valuestring;
                    char missing_for_event[256] = {0};
                    find_missing_characters(summary_utf8, missing_for_event,
                                            sizeof(missing_for_event));
                    if (strlen(missing_for_event) > 0) {
                        if (current_missing_len + strlen(missing_for_event) <= max_missing_len) {
                            strcat(missing_chars_total, missing_for_event);
                            current_missing_len += strlen(missing_for_event);
                        } else {
                            ESP_LOGW(TAG,
                                     "Total missing characters buffer full. Cannot add more from "
                                     "event '%s'.",
                                     summary_utf8);
                        }
                    }
                }

                // 2. Collect unique dates affected by this event
                cJSON *start_item = cJSON_GetObjectItemCaseSensitive(event_scanner_json, "start");
                cJSON *end_item = cJSON_GetObjectItemCaseSensitive(event_scanner_json, "end");
                if (cJSON_IsObject(start_item) && cJSON_IsObject(end_item)) {
                    cJSON *start_date_item_scan =
                        cJSON_GetObjectItemCaseSensitive(start_item, "date");
                    cJSON *end_date_item_scan = cJSON_GetObjectItemCaseSensitive(end_item, "date");
                    cJSON *start_datetime_item_scan =
                        cJSON_GetObjectItemCaseSensitive(start_item, "dateTime");
                    cJSON *end_datetime_item_scan =
                        cJSON_GetObjectItemCaseSensitive(end_item, "dateTime");

                    const char *start_date_str_scan =
                        cJSON_IsString(start_date_item_scan)
                            ? start_date_item_scan->valuestring
                            : (cJSON_IsString(start_datetime_item_scan)
                                   ? start_datetime_item_scan->valuestring
                                   : NULL);
                    const char *end_date_str_scan = cJSON_IsString(end_date_item_scan)
                                                        ? end_date_item_scan->valuestring
                                                        : (cJSON_IsString(end_datetime_item_scan)
                                                               ? end_datetime_item_scan->valuestring
                                                               : NULL);

                    if (start_date_str_scan && end_date_str_scan) {
                        struct tm start_tm_scan, end_tm_scan;
                        if (parse_date_string(start_date_str_scan, &start_tm_scan) &&
                            parse_date_string(end_date_str_scan, &end_tm_scan)) {
                            time_t start_t_scan = mktime(&start_tm_scan);
                            time_t end_t_scan = mktime(&end_tm_scan);
                            if (start_t_scan != (time_t)-1 && end_t_scan != (time_t)-1) {
                                for (time_t current_t_scan = start_t_scan;
                                     current_t_scan <= end_t_scan; current_t_scan += 86400) {
                                    struct tm current_tm_for_date_key;
                                    localtime_r(&current_t_scan, &current_tm_for_date_key);
                                    char date_key[11];
                                    strftime(date_key, sizeof(date_key), "%Y-%m-%d",
                                             &current_tm_for_date_key);

                                    bool found = false;
                                    for (int i = 0; i < unique_dates_count; i++) {
                                        if (strcmp(unique_dates[i], date_key) == 0) {
                                            found = true;
                                            break;
                                        }
                                    }
                                    if (!found && unique_dates_count < MAX_UNIQUE_DATES_PER_FETCH) {
                                        strcpy(unique_dates[unique_dates_count++], date_key);
                                    } else if (!found) {
                                        ESP_LOGW(TAG,
                                                 "Max unique dates limit (%d) reached, cannot add "
                                                 "%s for clearing.",
                                                 MAX_UNIQUE_DATES_PER_FETCH, date_key);
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Clear the .json files for all unique dates collected
            for (int i = 0; i < unique_dates_count; i++) {
                char file_to_delete_path[64];
                snprintf(file_to_delete_path, sizeof(file_to_delete_path), "%s/%s.json",
                         CALENDAR_DIR, unique_dates[i]);
                if (remove(file_to_delete_path) == 0) {
                    ESP_LOGI(TAG, "Cleared existing event file: %s", file_to_delete_path);
                } else {
                    if (errno != ENOENT) { // ENOENT is fine (file didn't exist)
                        ESP_LOGW(TAG, "Failed to delete %s: %s (errno %d)", file_to_delete_path,
                                 strerror(errno), errno);
                    }
                }
            }
            errno = 0; // Reset errno

            // Second pass: Save events to the (now cleared or new) daily files
            cJSON *event_json =
                NULL; // Re-declare for clarity or use a different name like event_saver_json
            cJSON_ArrayForEach(event_json, events_array) {
                if (!cJSON_IsObject(event_json)) {
                    ESP_LOGW(TAG, "Skipping non-object item in events array.");
                    continue;
                }

                // Process summary for missing characters
                // This was done in the first pass.

                // Process dates and save event
                cJSON *start_item = cJSON_GetObjectItemCaseSensitive(event_json, "start");
                cJSON *end_item = cJSON_GetObjectItemCaseSensitive(event_json, "end");

                if (cJSON_IsObject(start_item) && cJSON_IsObject(end_item)) {
                    // Prioritize 'date' for all-day events, otherwise use 'dateTime'
                    cJSON *start_date_item = cJSON_GetObjectItemCaseSensitive(start_item, "date");
                    cJSON *end_date_item = cJSON_GetObjectItemCaseSensitive(end_item, "date");
                    cJSON *start_datetime_item =
                        cJSON_GetObjectItemCaseSensitive(start_item, "dateTime");
                    cJSON *end_datetime_item =
                        cJSON_GetObjectItemCaseSensitive(end_item, "dateTime");

                    const char *start_date_str = cJSON_IsString(start_date_item)
                                                     ? start_date_item->valuestring
                                                     : (cJSON_IsString(start_datetime_item)
                                                            ? start_datetime_item->valuestring
                                                            : NULL);
                    const char *end_date_str =
                        cJSON_IsString(end_date_item)
                            ? end_date_item->valuestring
                            : (cJSON_IsString(end_datetime_item) ? end_datetime_item->valuestring
                                                                 : NULL);

                    if (start_date_str && end_date_str) {
                        struct tm start_tm, end_tm;
                        // Parse dates. Note: strptime handles both YYYY-MM-DD and
                        // YYYY-MM-DDTHH:MM:SS formats
                        if (parse_date_string(start_date_str, &start_tm) &&
                            parse_date_string(end_date_str, &end_tm)) {
                            // Convert struct tm to time_t for easier date arithmetic
                            time_t start_t = mktime(&start_tm);
                            time_t end_t = mktime(&end_tm);

                            if (start_t != (time_t)-1 && end_t != (time_t)-1) {
                                for (time_t current_t = start_t; current_t <= end_t;
                                     current_t += 86400) {
                                    struct tm current_tm_for_save;
                                    localtime_r(&current_t, &current_tm_for_save);
                                    char date_yyyymmdd[11];
                                    if (strftime(date_yyyymmdd, sizeof(date_yyyymmdd), "%Y-%m-%d",
                                                 &current_tm_for_save) > 0) {
                                        save_event_for_date(date_yyyymmdd, event_json);
                                    } else {
                                        ESP_LOGE(TAG, "Failed to format date for saving event.");
                                    }
                                }
                            } else {
                                ESP_LOGE(TAG, "Failed to convert parsed dates to time_t.");
                            }
                        } else {
                            ESP_LOGE(
                                TAG,
                                "Failed to parse start or end date string for event (save pass).");
                        }
                    } else {
                        ESP_LOGW(TAG, "Event missing start or end date/dateTime string.");
                    }
                }
            } // End cJSON_ArrayForEach
            // After processing all events, download missing characters if any
            download_missing_characters(missing_chars_total);
            ESP_LOGI(TAG, "Finished processing and saving calendar events.");
        } else {
            ESP_LOGW(TAG, "Events array is not valid or empty.");
        }
        cJSON_Delete(event->json_root); // Free the parsed JSON tree
        event->json_root = NULL;        // Mark as processed
    } else {
        ESP_LOGE(TAG, "Failed to receive calendar data or JSON parse error. HTTP result: %s",
                 esp_err_to_name(result));
        if (event->response_buffer && strlen(event->response_buffer) > 0) {
            ESP_LOGE(TAG, "Response: %s", event->response_buffer);
        }
        if (event->json_root) {
            cJSON_Delete(event->json_root);
            event->json_root = NULL;
        }
    }
}

void calendarStartup(void *pvParameters) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // 等待通知喚醒
        xEventGroupWaitBits(net_event_group, NET_TIME_AVAILABLE_BIT, false, true, portMAX_DELAY);
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
