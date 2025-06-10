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
// Static buffer for API responses related to calendar events
static char calendar_api_response_buffer[512];

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

    if (written != strlen(updated_content)) {
        ESP_LOGI(TAG, "written = %d, strlen(updated_content) = %d, updated content: %s", written,
                 strlen(updated_content), updated_content);
        ESP_LOGE(TAG, "Failed to write complete content to %s", file_path);
        return ESP_FAIL;
    }
    free(updated_content);

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

void collect_event_data_callback(net_event_t *event, esp_err_t result) {
    if (result == ESP_OK && event->json_root) {
        event_t ev_cal;
        ev_cal.event_id = SCREEN_EVENT_CALENDAR;
        if (event->user_data) {
            strncpy(ev_cal.msg, (char *)event->user_data, sizeof(ev_cal.msg) - 1);
            ev_cal.msg[sizeof(ev_cal.msg) - 1] = '\0';
            ESP_LOGI(TAG, "Sending SCREEN_EVENT_CALENDAR with date: %s", ev_cal.msg);
        } else {
            ESP_LOGW(TAG, "user_data (date) is NULL for calendar UI event. Sending with 'NoDate'.");
            snprintf(ev_cal.msg, sizeof(ev_cal.msg), "NoDate");
        }
        // Note: We send the UI event before freeing user_data,
        // as the queue send copies the event_t structure.

        // Free dynamically allocated post_data if it exists
        // This callback knows that for this specific event type, post_data was malloc'd.
        if (event->post_data) {
            free((void *)event->post_data);
            // Setting event->post_data to NULL is not strictly necessary here as the event
            // struct is a copy from the queue and will be discarded after this callback,
            // but it's good practice if the struct had a longer lifecycle.
            // event->post_data = NULL;
        }
        // Free dynamically allocated user_data (the date string)
        if (event->user_data) {
            free(event->user_data);
            // event->user_data = NULL; // Not strictly necessary for same reasons as post_data
        }

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

                const char *start_date_str_scan = NULL;
                if (start_item && cJSON_IsString(start_item)) {
                    start_date_str_scan = start_item->valuestring;
                }

                const char *end_date_str_scan = NULL;
                if (end_item && cJSON_IsString(end_item)) {
                    end_date_str_scan = end_item->valuestring;
                }

                if (start_date_str_scan && end_date_str_scan) {
                    struct tm start_tm_scan, end_tm_scan;
                    if (parse_date_string(start_date_str_scan, &start_tm_scan) &&
                        parse_date_string(end_date_str_scan, &end_tm_scan)) {
                        time_t start_t_scan = mktime(&start_tm_scan);
                        time_t end_t_scan = mktime(&end_tm_scan);
                        if (start_t_scan != (time_t)-1 && end_t_scan != (time_t)-1) {
                            for (time_t current_t_scan = start_t_scan; current_t_scan <= end_t_scan;
                                 current_t_scan += 86400) {
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
                } else {
                    ESP_LOGW(TAG, "Event scanner: 'start' or 'end' field missing or not a string.");
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

                cJSON *event_to_save = cJSON_Duplicate(event_json, true);
                if (!event_to_save) {
                    ESP_LOGE(TAG, "Failed to duplicate event_json for saving.");
                    continue;
                }

                // Process summary for missing characters
                // This was done in the first pass.

                // Process dates and save event
                cJSON *start_val_item = cJSON_GetObjectItemCaseSensitive(event_json, "start");
                cJSON *end_val_item = cJSON_GetObjectItemCaseSensitive(event_json, "end");

                const char *start_date_str = NULL;
                if (start_val_item && cJSON_IsString(start_val_item)) {
                    start_date_str = start_val_item->valuestring;
                }

                const char *end_date_str = NULL;
                if (end_val_item && cJSON_IsString(end_val_item)) {
                    end_date_str = end_val_item->valuestring;
                }

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
                                    save_event_for_date(date_yyyymmdd, event_to_save);
                                } else {
                                    ESP_LOGE(TAG, "Failed to format date for saving event.");
                                }
                            }
                        } else {
                            ESP_LOGE(TAG, "Failed to convert parsed dates to time_t.");
                        }
                    } else {
                        ESP_LOGE(TAG,
                                 "Failed to parse start or end date string for event (save pass).");
                    }
                } else {
                    ESP_LOGW(
                        TAG,
                        "Event missing 'start' or 'end' string fields, or they are not strings.");
                }
                cJSON_Delete(event_to_save); // Clean up the duplicated event
            } // End cJSON_ArrayForEach
            // After processing all events, download missing characters if any
            download_missing_characters(missing_chars_total);
            ESP_LOGI(TAG, "Finished processing and saving calendar events.");
            xQueueSend(gui_queue, &ev_cal, portMAX_DELAY); // Send the prepared UI event
        } else {
            ESP_LOGW(TAG, "Events array is not valid or empty.");
        }
        cJSON_Delete(event->json_root); // Free the parsed JSON tree
        event->json_root = NULL;        // Mark as processed
    } else {
        // Free dynamically allocated post_data even on failure
        if (event->post_data) {
            free((void *)event->post_data);
            event->post_data = NULL;
        }
        if (event->user_data) {
            free(event->user_data);
            event->user_data = NULL;
        }
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

void collect_event_data(time_t time) {
    char date[11];
    struct tm timeinfo;
    localtime_r(&time, &timeinfo);
    strftime(date, sizeof(date), "%Y-%m-%d", &timeinfo);

    // Dynamically allocate buffer for post_data
    char *dynamic_post_data = malloc(27); // Approximate size for {"date":"YYYY-MM-DD"}
    if (!dynamic_post_data) {
        ESP_LOGE(TAG, "Failed to allocate memory for post_data");
        return;
    }
    snprintf(dynamic_post_data, 27, "{\"date\":\"%s\"}", date);

    // Duplicate the date string for the UI event message
    char *date_for_ui_event = strdup(date); // strdup uses malloc
    if (!date_for_ui_event) {
        ESP_LOGE(TAG, "Failed to allocate memory for date_for_ui_event");
        free(dynamic_post_data); // Clean up previously allocated memory
        return;
    }

    // Clear the static response buffer before use
    calendar_api_response_buffer[0] = '\0';

    net_event_t event = {
        .url = CALENDAR_URL,
        .method = HTTP_METHOD_POST,
        .post_data = dynamic_post_data, // Use dynamically allocated buffer
        .use_jwt = true,
        .save_to_buffer = true,
        .response_buffer = calendar_api_response_buffer, // Use static calendar response buffer
        .response_buffer_size = sizeof(calendar_api_response_buffer),
        .on_finish = collect_event_data_callback,
        .user_data = date_for_ui_event, // Pass the date string for the UI
        .json_parse = 1,
    };
    xQueueSend(net_queue, &event, portMAX_DELAY);
}

void calendar_startup(void) {
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
        time_t now;
        time(&now);
        collect_event_data(now);
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
