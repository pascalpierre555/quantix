#include "EC11_driver.h"
#include "cJSON.h"
#include "driver/gpio.h"   // For GPIO configuration
#include "driver/rtc_io.h" // For RTC GPIO pull-up/down control
#include "esp_log.h"
#include "esp_sleep.h" // For deep sleep
#include "esp_sntp.h"  // ESP_SNTP_OPMODE_POLL etc.
#include "font_task.h"
#include "freertos/semphr.h"
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

/** @brief Log tag for general calendar operations. */
#define TAG_CALENDAR "CALENDAR"
/** @brief Log tag for the deep sleep manager. */
#define TAG_SLEEP_MGR "SLEEP_MGR"
/** @brief URL for the backend calendar API. */
#define CALENDAR_URL "https://peng-pc.tail941dce.ts.net/api/calendar"

/** @brief Directory in LittleFS for storing cached calendar event files. */
#define CALENDAR_DIR "/littlefs/calendar"

/** @brief Notification value used by the encoder callback to signify a command. */
#define CALENDAR_NOTIFY_INDEX_CMD 0
/** @brief Notification value used by network callbacks to signify data is ready. */
#define CALENDAR_NOTIFY_INDEX_DATA_READY 1

/** @brief Static buffer for storing API responses related to calendar events. */
static char calendar_api_response_buffer[512];
/** @brief Task handle for the calendar data prefetch task. */
TaskHandle_t xCalendarPrefetchHandle;
/** @brief Task handle for the calendar display/interaction task. */
TaskHandle_t xCalendarDisplayHandle = NULL;

/** @brief Log tag for the prefetch mechanism. */
#define TAG_PREFETCH "PREFETCH_CAL"
/** @brief Number of days to cache prefetch status for (+/- 5 days). */
#define MAX_PREFETCH_CACHE_SIZE 11
/** @brief Cooldown period in seconds before re-fetching data for a cached date. */
#define PREFETCH_COOLDOWN_SECONDS (5 * 60)

/** @brief Structure to hold prefetch cache information for a specific date. */
typedef struct {
    struct tm date_t;     /**< The date (normalized to midnight). */
    time_t last_fetch_ts; /**< Timestamp of the last fetch for this date. */
} PrefetchCacheEntry;

/** @brief Array-based cache to track recently prefetched dates. */
static PrefetchCacheEntry prefetch_cache[MAX_PREFETCH_CACHE_SIZE];
/** @brief The current number of valid entries in the prefetch cache. */
static int prefetch_cache_fill_count = 0;
/** @brief The index to use for the next replacement in the circular cache. */
static int prefetch_cache_next_replace_idx = 0;

/** @brief Mutex to protect access to the prefetch cache. */
static SemaphoreHandle_t xPrefetchCacheMutex = NULL;

/**
 * @brief The date currently being displayed, stored in RTC memory to survive deep sleep.
 */
RTC_DATA_ATTR struct tm current_display_time;

/** @brief Event group used to coordinate sleep state transitions. */
EventGroupHandle_t sleep_event_group;
/** @brief Bit in `sleep_event_group` indicating that a task has requested deep sleep. */
#define DEEP_SLEEP_REQUESTED_BIT (1 << 0)

/** @brief Extern reference from font_task.c to check font table usage. */
extern int font_table_count;

/**
 * @brief Parses a date string into a `struct tm`.
 *
 * This function attempts to parse a string in either "YYYY-MM-DDTHH:MM:SS" or "YYYY-MM-DD" format.
 * @param date_str The input date string.
 * @param tm_out Pointer to a `struct tm` to store the parsed result.
 * @return `true` on successful parsing, `false` otherwise.
 */
bool parse_date_string(const char *date_str, struct tm *tm_out) {
    if (!date_str || !tm_out)
        return false;

    memset(tm_out, 0, sizeof(struct tm));

    // Try parsing YYYY-MM-DDTHH:MM:SS...
    char *parse_end = strptime(date_str, "%Y-%m-%dT%H:%M:%S", tm_out);
    if (parse_end && (*parse_end == '+' || *parse_end == '-' || *parse_end == 'Z')) {
        // Successfully parsed YYYY-MM-DDTHH:MM:SS. We can stop here for the date part.
        return true;
    }

    // If not YYYY-MM-DDTHH:MM:SS..., try parsing YYYY-MM-DD
    parse_end = strptime(date_str, "%Y-%m-%d", tm_out);
    if (parse_end && *parse_end == '\0') {
        // Successfully parsed YYYY-MM-DD
        return true;
    }

    // If parsing fails, try setting tm_isdst to -1 (unknown) to let mktime decide.
    if (tm_out->tm_year == 0 && tm_out->tm_mon == 0 && tm_out->tm_mday == 0) {
        tm_out->tm_isdst = -1; // Let mktime determine daylight saving time
    }

    ESP_LOGE(TAG_CALENDAR, "Failed to parse date string: %s", date_str);
    return false;
}

/**
 * @brief Saves a single event to its corresponding date file in LittleFS.
 *
 * This function manages a JSON file for each day (e.g., "2023-10-27.json").
 * It reads the existing file (if any), parses it as a JSON array, checks if the
 * new event (by summary and start time) already exists, and if not, appends it.
 * The entire updated array is then written back to the file, overwriting the old one.
 *
 * @param date_yyyymmdd The date string ("YYYY-MM-DD") which determines the filename.
 * @param event_json A cJSON object representing the single event to be saved.
 * @return ESP_OK on success, or an esp_err_t code on failure.
 */
esp_err_t save_event_for_date(const char *date_yyyymmdd, const cJSON *event_json) {
    if (!date_yyyymmdd || !event_json) {
        return ESP_ERR_INVALID_ARG;
    }

    // Construct file path: CALENDAR_DIR/YYYY-MM-DD.json
    char file_path[64];
    snprintf(file_path, sizeof(file_path), "%s/%s.json", CALENDAR_DIR, date_yyyymmdd);

    cJSON *events_for_day = NULL;
    char *existing_content = NULL;
    FILE *f = fopen(file_path, "rb"); // Open in read-binary mode

    if (f) {
        // File exists, read its content
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
            events_for_day = cJSON_CreateArray(); // File is empty
        }
        fclose(f);
        free(existing_content);
    } else {
        events_for_day = cJSON_CreateArray(); // File does not exist
    }

    if (!events_for_day) {
        ESP_LOGE(TAG_CALENDAR, "Failed to get or create JSON array for %s", file_path);
        return ESP_FAIL;
    }

    // Check if an event with the same summary and start time already exists in the array
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
            cJSON_Delete(events_for_day); // Clean up before returning
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(events_for_day, event_copy);
    }

    char *updated_content = cJSON_PrintUnformatted(events_for_day);
    cJSON_Delete(events_for_day);

    if (!updated_content) {
        ESP_LOGE(TAG_CALENDAR, "Failed to print updated JSON array.");
        return ESP_FAIL;
    }

    // Write the updated content back to the file (overwrite)
    f = fopen(file_path, "wb");
    if (!f) {
        ESP_LOGE(TAG_CALENDAR, "Failed to open file for writing: %s", file_path);
        free(updated_content);
        return ESP_FAIL;
    }

    size_t content_len = strlen(updated_content);
    size_t written = fwrite(updated_content, 1, content_len, f);
    fclose(f);

    if (written != content_len) {
        ESP_LOGE(TAG_CALENDAR, "Failed to write full content to %s (wrote %zu / expected %zu)",
                 file_path, written, content_len);
        return ESP_FAIL;
    }
    free(updated_content); // Free memory allocated by cJSON_PrintUnformatted

    if (!event_exists) {
        ESP_LOGI(TAG_CALENDAR, "New event saved to %s", file_path);
    }
    return ESP_OK;
}

/**
 * @brief Checks if essential calendar settings (e.g., email) exist in NVS.
 *
 * This is used to determine if the device has been configured with a Google account.
 *
 * @return ESP_OK if the "email" key is found, or an esp_err_t code on failure or if not found.
 */
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

/**
 * @brief Callback function to process the response from a calendar data request.
 *
 * This function is called by the `net_worker_task` after an HTTP request for calendar
 * events completes. It performs the following steps:
 * 1. Parses the JSON response to get an array of events.
 * 2. Clears the old event file for the requested date to ensure a full refresh.
 * 3. Iterates through the new events:
 *    a. Checks the event summary for any Chinese characters whose fonts are not cached locally.
 *    b. Aggregates all missing font characters and queues a request to download them.
 *    c. Saves each new event to the corresponding date file in LittleFS.
 * 4. Sets an event group bit to signal that calendar data is available.
 * 5. Frees the memory allocated for the network request's `post_data` and `user_data`.
 *
 * @param event The network event structure containing the response and other data.
 * @param result The result of the HTTP request (ESP_OK on success).
 */
void collect_event_data_callback(net_event_t *event, esp_err_t result) {
    char date[11] = {0};
    if (result == ESP_OK && event->json_root) {
        if (event->user_data) {
            strncpy(date, (char *)event->user_data, sizeof(date) - 1);
        } else {
            ESP_LOGW(TAG_CALENDAR, "user_data (date) for calendar UI event is NULL.");
        }

        // Free the dynamically allocated post_data if it exists.
        // This callback knows that for this specific event type, post_data was malloc'd.
        if (event->post_data) {
            free((void *)event->post_data);
        }
        // Free the dynamically allocated user_data (the date string).
        if (event->user_data) {
            free(event->user_data);
        }

        ESP_LOGI(TAG_CALENDAR, "Received calendar data, processing events...");
        cJSON *events_array = cJSON_GetObjectItemCaseSensitive(event->json_root, "events");

        if (cJSON_IsArray(events_array)) {
            char missing_chars_total[256] = {0};
            size_t current_missing_len = 0;
            const size_t max_missing_len = sizeof(missing_chars_total) - (HEX_KEY_LEN - 1) - 1;

            cJSON *event_scanner_json = NULL;

            // Before saving new events, delete the old file for this date to ensure a clean slate.
            char file_to_delete_path[64];
            snprintf(file_to_delete_path, sizeof(file_to_delete_path), "%s/%s.json", CALENDAR_DIR,
                     date);
            if (remove(file_to_delete_path) == 0) {
                ESP_LOGI(TAG_CALENDAR, "Cleared existing event file: %s", file_to_delete_path);
            } else {
                // ENOENT (No such file or directory) is normal, means the file didn't exist.
                if (errno != ENOENT) {
                    ESP_LOGW(TAG_CALENDAR, "Failed to delete %s: %s (errno %d)",
                             file_to_delete_path, strerror(errno), errno);
                }
            }
            errno = 0; // Reset errno

            // Iterate through all received events for the date.
            cJSON_ArrayForEach(event_scanner_json, events_array) {
                if (!cJSON_IsObject(event_scanner_json)) {
                    ESP_LOGW(TAG_CALENDAR, "Skipping non-object item in events array (scan pass).");
                    continue;
                }

                // 1. Process the summary to find missing font characters.
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
                            // If too many missing fonts are found, download the current batch first.
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

                // 2. Save the event to the file system.
                cJSON *event_to_save = cJSON_Duplicate(event_scanner_json, true);
                if (!event_to_save) {
                    ESP_LOGE(TAG_CALENDAR, "Failed to duplicate event_json for saving.");
                    continue;
                }
                save_event_for_date(date, event_to_save);
                cJSON_Delete(event_to_save); // Clean up the duplicated event
            }

            ESP_LOGI(TAG_CALENDAR,
                     "Finished processing and saving calendar events. Notifying on index %d.",
                     CALENDAR_NOTIFY_INDEX_DATA_READY);
            xEventGroupSetBits(net_event_group, NET_CALENDAR_AVAILABLE_BIT);
        } else {
            ESP_LOGW(TAG_CALENDAR, "Events array is invalid or empty.");
        }
        cJSON_Delete(event->json_root); // Free the parsed JSON tree
        event->json_root = NULL;        // Mark as processed
    } else {
        // Even on failure, free the dynamically allocated post_data
        if (event->post_data) {
            free((void *)event->post_data);
            event->post_data = NULL;
        }
        // Even on failure, free the dynamically allocated user_data
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
        // If json_root is not NULL (e.g., parsing was done in net_task but another error was
        // detected in the callback)
        if (event->json_root) {
            cJSON_Delete(event->json_root);
            event->json_root = NULL;
        }
    }
}

/**
 * @brief Queues a network request to fetch calendar events for a specific date.
 *
 * This function constructs a `net_event_t` to request calendar data for the given
 * `timeinfo`. It dynamically allocates memory for the POST data and the `user_data`
 * (which carries the date string for the callback). The request is then sent to
 * the `net_queue` to be processed by the `net_worker_task`.
 *
 * @param timeinfo A `struct tm` representing the date for which to fetch events.
 */
void collect_event_data(struct tm timeinfo) {
    char date[11];
    strftime(date, sizeof(date), "%Y-%m-%d", &timeinfo);

    // Dynamically allocate buffer for post_data, e.g., {"date":"YYYY-MM-DD"} needs ~27 bytes.
    char *dynamic_post_data = malloc(27);
    if (!dynamic_post_data) {
        ESP_LOGE(TAG_CALENDAR, "Failed to allocate memory for post_data");
        return;
    }
    snprintf(dynamic_post_data, 27, "{\"date\":\"%s\"}", date);

    // Duplicate the date string for the UI event message.
    char *date_for_ui_event = strdup(date); // strdup uses malloc
    if (!date_for_ui_event) {
        ESP_LOGE(TAG_CALENDAR, "Failed to allocate memory for date_for_ui_event");
        free(dynamic_post_data); // Clean up previously allocated memory
        return;
    }

    // 使用前清除靜態回應緩衝區
    calendar_api_response_buffer[0] = '\0';

    net_event_t event = {
        .url = CALENDAR_URL,
        .method = HTTP_METHOD_POST,
        .post_data = dynamic_post_data, // Use the dynamically allocated buffer.
        .use_jwt = true,
        .save_to_buffer = true,
        .response_buffer = calendar_api_response_buffer, // Use the static calendar response buffer.
        .response_buffer_size = sizeof(calendar_api_response_buffer),
        .on_finish = collect_event_data_callback,
        .user_data = date_for_ui_event, // Pass the date string to the UI.
        .json_parse = 1,                // Request net_task to parse the JSON.
    };
    xQueueSend(net_queue, &event, portMAX_DELAY);
}

/**
 * @brief Manages the transition to deep sleep.
 *
 * This task waits for the `DEEP_SLEEP_REQUESTED_BIT` to be set in the `sleep_event_group`.
 * Once requested, it continuously checks if the system is idle (e.g., queues are empty,
 * critical semaphores are available). When all conditions are met, it configures the
 * wakeup sources (GPIO pins) and puts the device into deep sleep.
 *
 * @param pvParameters Unused.
 */
void deep_sleep_manager_task(void *pvParameters) {
    const TickType_t check_interval = pdMS_TO_TICKS(2000); // Check conditions every 2 seconds.
    ESP_LOGI(TAG_SLEEP_MGR, "Deep Sleep Manager task started.");

    for (;;) {
        // Wait until sleep is requested.
        ESP_LOGI(TAG_SLEEP_MGR, "Waiting for deep sleep request...");
        xEventGroupWaitBits(sleep_event_group, DEEP_SLEEP_REQUESTED_BIT, pdFALSE, pdTRUE,
                            portMAX_DELAY);
        ESP_LOGI(TAG_SLEEP_MGR, "Deep sleep request received. Starting checks...");

        bool can_sleep_now = false;

        // Keep checking conditions as long as the sleep request is active.
        while (xEventGroupGetBits(sleep_event_group) & DEEP_SLEEP_REQUESTED_BIT) {
            can_sleep_now = false; // Reset status for this iteration.

            // 1. 檢查佇列 (非阻塞)
            if (uxQueueMessagesWaiting(gui_queue) != 0) {
                ESP_LOGD(TAG_SLEEP_MGR, "GUI queue not empty. Postponing sleep check.");
                vTaskDelay(check_interval);
                continue;
            }
            if (uxQueueMessagesWaiting(net_queue) != 0) {
                ESP_LOGD(TAG_SLEEP_MGR, "Net queue not empty. Postponing sleep check.");
                vTaskDelay(check_interval);
                continue;
            }
            ESP_LOGD(TAG_SLEEP_MGR, "Queues are empty.");

            // 2. Try to acquire semaphores with a short timeout.
            bool screen_locked = false;
            bool wifi_locked = false;

            if (xSemaphoreTake(xScreen, pdMS_TO_TICKS(50)) == pdTRUE) {
                screen_locked = true;
                ESP_LOGD(TAG_SLEEP_MGR, "Screen semaphore acquired.");
                if (xSemaphoreTake(xWifi, pdMS_TO_TICKS(50)) == pdTRUE) {
                    wifi_locked = true;
                    ESP_LOGD(TAG_SLEEP_MGR, "WiFi semaphore acquired.");
                    can_sleep_now = true;
                } else {
                    ESP_LOGD(TAG_SLEEP_MGR,
                             "Could not acquire WiFi semaphore. Releasing screen lock.");
                    xSemaphoreGive(xScreen); // Release screen lock if wifi acquire fails.
                    screen_locked = false;
                }
            } else {
                ESP_LOGD(TAG_SLEEP_MGR, "Could not acquire screen semaphore.");
            }

            if (can_sleep_now) {
                ESP_LOGI(TAG_SLEEP_MGR,
                         "All conditions met. Proceeding to deep sleep configuration.");

                // 3. Configure wakeup sources.
                uint64_t ext1_wakeup_pins_mask =
                    (1ULL << PIN_BUTTON) | (1ULL << PIN_ENCODER_A) | (1ULL << PIN_ENCODER_B);
                esp_sleep_ext1_wakeup_mode_t wakeup_mode = ESP_EXT1_WAKEUP_ANY_LOW;
                esp_err_t err_wakeup =
                    esp_sleep_enable_ext1_wakeup(ext1_wakeup_pins_mask, wakeup_mode);

                if (err_wakeup != ESP_OK) {
                    ESP_LOGE(TAG_SLEEP_MGR,
                             "Failed to enable ext1 wakeup: %s. Aborting sleep attempt.",
                             esp_err_to_name(err_wakeup));
                    if (wifi_locked)
                        xSemaphoreGive(xWifi);
                    if (screen_locked)
                        xSemaphoreGive(xScreen);
                    vTaskDelay(check_interval); // Wait and retry setting.
                    continue;                   // Continue while loop to re-check conditions.
                }

                ec11_clean_button_callback();
                ec11_clean_encoder_callback();
                if (rtc_gpio_is_valid_gpio(PIN_BUTTON)) {
                    rtc_gpio_pullup_en(PIN_BUTTON);
                    rtc_gpio_pulldown_dis(PIN_BUTTON);
                }
                if (rtc_gpio_is_valid_gpio(PIN_ENCODER_A)) {
                    rtc_gpio_pullup_en(PIN_ENCODER_A);
                    rtc_gpio_pulldown_dis(PIN_ENCODER_A);
                }
                if (rtc_gpio_is_valid_gpio(PIN_ENCODER_B)) {
                    rtc_gpio_pullup_en(PIN_ENCODER_B);
                    rtc_gpio_pulldown_dis(PIN_ENCODER_B);
                }

                ESP_LOGI(TAG_SLEEP_MGR, "Entering deep sleep NOW.");
                vTaskDelay(pdMS_TO_TICKS(100)); // Ensure log is flushed.
                esp_deep_sleep_start();
                // Code will not reach here if esp_deep_sleep_start() is successful.
            } else {
                ESP_LOGD(TAG_SLEEP_MGR, "Conditions not met for sleep. Retrying in %d ms.",
                         (int)pdTICKS_TO_MS(check_interval));
                vTaskDelay(check_interval);
            }
        }
        ESP_LOGI(TAG_SLEEP_MGR, "Sleep request cancelled or processed. Waiting for new request.");
    }
}

/**
 * @brief Checks if a given date should be prefetched based on cache status and cooldown.
 *
 * This helper function determines whether to initiate a network request for a specific date.
 * It checks a local cache (`prefetch_cache`) to see if the date has been fetched recently.
 * - If the date is in the cache and was fetched within the `PREFETCH_COOLDOWN_SECONDS` period, it
 * returns `false`.
 * - If the date is in the cache but the cooldown has expired, it updates the timestamp and returns
 * `true`.
 * - If the date is not in the cache, it adds it to the cache (using a circular replacement
 *   strategy if full) and returns `true`.
 *
 * This function is thread-safe and uses `xPrefetchCacheMutex` to protect the cache.
 *
 * @param target_date_ts A `struct tm` representing the date to check.
 * @return `true` if the date should be prefetched, `false` otherwise.
 */
static bool should_prefetch_date(struct tm target_date_ts) {
    char date_str_log[12];
    strftime(date_str_log, sizeof(date_str_log), "%Y-%m-%d", &target_date_ts);

    if (xSemaphoreTake(xPrefetchCacheMutex, portMAX_DELAY) == pdFALSE) {
        ESP_LOGE(TAG_PREFETCH, "Failed to take prefetch cache mutex");
        return false; // Don't prefetch if mutex cannot be acquired.
    }

    time_t current_sys_time;
    time(&current_sys_time);

    for (int i = 0; i < prefetch_cache_fill_count; ++i) {
        if (prefetch_cache[i].date_t.tm_year == target_date_ts.tm_year &&
            prefetch_cache[i].date_t.tm_mon == target_date_ts.tm_mon &&
            prefetch_cache[i].date_t.tm_mday == target_date_ts.tm_mday) {
            // Date found in cache, check cooldown.
            if ((current_sys_time - prefetch_cache[i].last_fetch_ts) < PREFETCH_COOLDOWN_SECONDS) {
                xSemaphoreGive(xPrefetchCacheMutex);
                ESP_LOGI(TAG_PREFETCH, "Date %s already fetched within %d mins. Skipping.",
                         date_str_log, PREFETCH_COOLDOWN_SECONDS / 60);
                return false; // Recently fetched.
            } else {
                // Cooldown expired, update timestamp and re-fetch.
                prefetch_cache[i].last_fetch_ts = current_sys_time;
                xSemaphoreGive(xPrefetchCacheMutex);
                ESP_LOGI(TAG_PREFETCH, "Date %s found in cache, older than %d mins. Refetching.",
                         date_str_log, PREFETCH_COOLDOWN_SECONDS / 60);
                return true;
            }
        }
    }

    // Not in cache, add it.
    if (prefetch_cache_fill_count < MAX_PREFETCH_CACHE_SIZE) {
        // Add to the cache if there is space.
        prefetch_cache[prefetch_cache_fill_count].date_t = target_date_ts;
        prefetch_cache[prefetch_cache_fill_count].last_fetch_ts = current_sys_time;
        prefetch_cache_fill_count++;
        ESP_LOGI(TAG_PREFETCH, "Date %s not in cache. Added. Fetching.", date_str_log);
    } else {
        // Cache is full, use circular replacement strategy.
        ESP_LOGW(TAG_PREFETCH, "Prefetch cache full. Replacing entry at index %d for date %s.",
                 prefetch_cache_next_replace_idx, date_str_log);
        prefetch_cache[prefetch_cache_next_replace_idx].date_t = target_date_ts;
        prefetch_cache[prefetch_cache_next_replace_idx].last_fetch_ts = current_sys_time;
        prefetch_cache_next_replace_idx =
            (prefetch_cache_next_replace_idx + 1) % MAX_PREFETCH_CACHE_SIZE;
    }
    xSemaphoreGive(xPrefetchCacheMutex);
    return true;
}

// 日曆模塊啟動函數
void calendar_prefetch_task(void *pvParameters) {
    // 等待 WiFi 連接
    xEventGroupWaitBits(net_event_group, NET_WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
    // 設定 NTP 伺服器和操作模式
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    // 等待NTP同步完成
    ESP_LOGI(TAG_CALENDAR, "Waiting for NTP time synchronization...");
    time_t now;
    // 初始化 timeinfo 避免在 sntp_get_sync_status() 返回 SNTP_SYNC_STATUS_RESET 時 localtime_r 出錯
    memset(&current_display_time, 0, sizeof(struct tm));

    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET ||
           current_display_time.tm_year < (2023 - 1900)) {
        vTaskDelay(2000 / portTICK_PERIOD_MS); // 每2秒檢查一次
        time(&now);
        localtime_r(&now, &current_display_time);
        ESP_LOGI(TAG_CALENDAR, "Current time: %04d-%02d-%02d %02d:%02d:%02d, waiting for sync...",
                 current_display_time.tm_year + 1900, current_display_time.tm_mon + 1,
                 current_display_time.tm_mday, current_display_time.tm_hour,
                 current_display_time.tm_min, current_display_time.tm_sec);
    }
    xEventGroupWaitBits(net_event_group, NET_SERVER_CONNECTED_BIT, false, true, portMAX_DELAY);
    if (check_calendar_settings() != ESP_OK) {
        xEventGroupClearBits(net_event_group, NET_GOOGLE_TOKEN_AVAILABLE_BIT);
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
    xPrefetchCacheMutex = xSemaphoreCreateMutex();
    memset(prefetch_cache, 0, sizeof(prefetch_cache)); // 初始化快取
    if (xPrefetchCacheMutex == NULL) {                 // 確保互斥鎖已創建
        xPrefetchCacheMutex = xSemaphoreCreateMutex();
        if (xPrefetchCacheMutex == NULL) {
            ESP_LOGE(TAG_PREFETCH, "Failed to create prefetch cache mutex in task!");
            vTaskDelete(NULL);
        }
        memset(prefetch_cache, 0, sizeof(prefetch_cache));
    }
    for (;;) {
        xTaskNotifyWait(pdFALSE, pdTRUE, NULL, portMAX_DELAY);

        xEventGroupWaitBits(net_event_group, NET_GOOGLE_TOKEN_AVAILABLE_BIT, false, true,
                            portMAX_DELAY);
        // 在等待新命令前，清除睡眠請求，因為我們即將處理新命令
        xEventGroupClearBits(sleep_event_group, DEEP_SLEEP_REQUESTED_BIT);

        // 檢查字體表快取使用情況
        // MAX_FONTS 是在 font_task.h 中定義的
        if (font_table_count > (MAX_FONTS * 3 / 4)) { // 超過 75%
            ESP_LOGI(TAG_PREFETCH, "Font table cache is >75%% full (%d/%d). Clearing font table.",
                     font_table_count, MAX_FONTS);
            font_table_init(); // 清空字體表 (此函數也會重置 font_table_count)

            // 清空字體表後，重置 prefetch_cache 中所有條目的 last_fetch_ts 以強制重新預取
            if (xSemaphoreTake(xPrefetchCacheMutex, portMAX_DELAY) == pdTRUE) {
                ESP_LOGI(TAG_PREFETCH, "Resetting prefetch cache timestamps to force re-fetch.");
                for (int i = 0; i < prefetch_cache_fill_count; ++i) {
                    prefetch_cache[i].last_fetch_ts = 0; // 設為0以確保下次檢查時會重新擷取
                }
                xSemaphoreGive(xPrefetchCacheMutex);
            } else {
                ESP_LOGE(TAG_PREFETCH,
                         "Failed to take prefetch cache mutex for resetting timestamps.");
            }
        }

        ESP_LOGI(TAG_PREFETCH, "Waiting for prefetch trigger notification");
        // 等待來自 calendar_startup 的通知，其中包含中心日期的 time_t

        ESP_LOGI(TAG_PREFETCH, "Received prefetch trigger for date: %04d-%02d-%02d",
                 current_display_time.tm_year + 1900, current_display_time.tm_mon + 1,
                 current_display_time.tm_mday);

        // 在開始預取前，如果之前有睡眠請求，先取消它，因為我們現在要忙了
        ESP_LOGI(TAG_PREFETCH, "Clearing deep sleep request before starting prefetch cycle.");

        struct tm day_to_fetch_t = current_display_time;
        if (should_prefetch_date(day_to_fetch_t)) {
            ESP_LOGI(TAG_PREFETCH, "fetching for today : %04d-%02d-%02d",
                     day_to_fetch_t.tm_year + 1900, day_to_fetch_t.tm_mon + 1,
                     day_to_fetch_t.tm_mday);
            collect_event_data(day_to_fetch_t); // 非同步呼叫
        }

        for (int offset = 1; offset <= 5; ++offset) {
            // 檢查是否有新的通知以中斷當前預取序列
            if (xTaskNotifyWait(0, ULONG_MAX, NULL, 0) == pdPASS) {
                ESP_LOGI(TAG_PREFETCH,
                         "Prefetch cycle interrupted by new date. Clearing sleep request.");
                xEventGroupClearBits(sleep_event_group, DEEP_SLEEP_REQUESTED_BIT);
                ESP_LOGI(TAG_PREFETCH,
                         "Prefetch interrupted by new trigger for date: %04d-%02d-%02d. "
                         "Restarting prefetch.",
                         current_display_time.tm_year + 1900, current_display_time.tm_mon + 1,
                         current_display_time.tm_mday);
                offset = 0; // 會在下一次迭代變成 1，重新開始
                xEventGroupWaitBits(net_event_group, NET_GOOGLE_TOKEN_AVAILABLE_BIT, pdFALSE,
                                    pdTRUE,
                                    portMAX_DELAY); // 再次檢查
                continue;
            }

            // 預取 後一天 (current_center_day_t + offset)
            day_to_fetch_t = current_display_time;
            day_to_fetch_t.tm_mday += offset;
            mktime(&day_to_fetch_t);
            if (should_prefetch_date(day_to_fetch_t)) {
                ESP_LOGI(TAG_PREFETCH, "Prefetching for next day (+%d): %04d-%02d-%02d", offset,
                         day_to_fetch_t.tm_year + 1900, day_to_fetch_t.tm_mon + 1,
                         day_to_fetch_t.tm_mday);
                collect_event_data(day_to_fetch_t); // 非同步呼叫
            }

            // 再次檢查中斷
            if (xTaskNotifyWait(0, ULONG_MAX, NULL, 0) == pdPASS) {
                ESP_LOGI(TAG_PREFETCH,
                         "Prefetch cycle interrupted by new date. Clearing sleep request.");
                xEventGroupClearBits(sleep_event_group, DEEP_SLEEP_REQUESTED_BIT);
                ESP_LOGI(TAG_PREFETCH,
                         "Prefetch interrupted by new trigger for date: %04d-%02d-%02d. "
                         "Restarting prefetch.",
                         current_display_time.tm_year + 1900, current_display_time.tm_mon + 1,
                         current_display_time.tm_mday);
                offset = 0;
                xEventGroupWaitBits(net_event_group, NET_GOOGLE_TOKEN_AVAILABLE_BIT, pdFALSE,
                                    pdTRUE, portMAX_DELAY);
                continue;
            }

            // 預取 前一天 (current_center_day_t - offset)
            day_to_fetch_t = current_display_time;
            day_to_fetch_t.tm_mday -= offset;
            mktime(&day_to_fetch_t);
            if (should_prefetch_date(day_to_fetch_t)) {
                ESP_LOGI(TAG_PREFETCH, "Prefetching for next day (-%d): %04d-%02d-%02d", offset,
                         day_to_fetch_t.tm_year + 1900, day_to_fetch_t.tm_mon + 1,
                         day_to_fetch_t.tm_mday);
                collect_event_data(day_to_fetch_t); // 非同步呼叫
            }

            vTaskDelay(pdMS_TO_TICKS(200)); // 短暫延遲以允許其他任務執行，並避免過於頻繁的API請求
        }
        ESP_LOGI(TAG_PREFETCH, "Finished prefetch cycle for center date %04d-%02d-%02d",
                 current_display_time.tm_year + 1900, current_display_time.tm_mon + 1,
                 current_display_time.tm_mday);

        // 預取完成後，請求進入睡眠
        ESP_LOGI(TAG_PREFETCH, "Prefetch cycle complete. Requesting deep sleep.");
        xEventGroupSetBits(sleep_event_group, DEEP_SLEEP_REQUESTED_BIT);
        ec11_set_encoder_callback(xCalendarDisplayHandle);
    }
}

/**
 * @brief Task to handle calendar display and user interaction.
 *
 * This task waits for notifications from the rotary encoder to change the displayed date.
 * When a notification is received, it updates the `current_display_time`, sends an event
 * to the UI task to redraw the screen, and triggers the `calendar_prefetch_task` to
 * fetch new data.
 *
 * @param pvParameters Unused.
 */
void calendar_display(void *pvParameters) {
    uint32_t time_shift = 0;
    for (;;) {
        time_shift = 0;
        // Wait for a command notification (from EC11 or an initial signal).
        ESP_LOGI(TAG_CALENDAR, "Waiting for command notification on index %d",
                 CALENDAR_NOTIFY_INDEX_CMD);
        // Before waiting for a new command, clear the sleep request.
        ESP_LOGI(TAG_CALENDAR, "Clearing deep sleep request before waiting for new command.");
        xTaskNotifyWait(pdTRUE, pdTRUE, &time_shift, portMAX_DELAY);
        xEventGroupClearBits(sleep_event_group, DEEP_SLEEP_REQUESTED_BIT);
        xEventGroupWaitBits(net_event_group, NET_CALENDAR_AVAILABLE_BIT, pdFALSE, pdTRUE,
                            portMAX_DELAY);
        ESP_LOGI(TAG_CALENDAR, "Hi");
        switch (time_shift) {
        case 2:
            current_display_time.tm_mday += 1;
            break;
        case 1:
            current_display_time.tm_mday -= 1;
            break;
        default:
            break;
        }
        // Normalize current_display_time (handles month/year carry-over).
        mktime(&current_display_time);

        ESP_LOGI(TAG_CALENDAR,
                 "Time shift processed, value: %" PRIu32
                 ". Current date for data collection: %04d-%02d-%02d",
                 time_shift, current_display_time.tm_year + 1900, current_display_time.tm_mon + 1,
                 current_display_time.tm_mday);
        event_t ev = {
            .event_id = SCREEN_EVENT_CALENDAR,
        };
        strftime(ev.msg, 11, "%Y-%m-%d", &current_display_time);
        xQueueSend(gui_queue, &ev, portMAX_DELAY);
        if (xEventGroupGetBits(net_event_group) & NET_SERVER_CONNECTED_BIT) {
            xTaskNotifyGive(xCalendarPrefetchHandle);
        }
        ESP_LOGI(TAG_CALENDAR, "Setting encoder callback to notify task %p on index %d",
                 xCalendarDisplayHandle, CALENDAR_NOTIFY_INDEX_CMD);
        // After processing the current date, request to enter sleep.
        ESP_LOGI(TAG_CALENDAR, "Date processing complete. Requesting deep sleep.");
        ec11_set_encoder_callback(xCalendarDisplayHandle);
        vTaskDelay(10);
    }
}