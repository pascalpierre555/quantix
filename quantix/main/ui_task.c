#include "ui_task.h"
#include "EC11_driver.h"
#include "EPD_2in9.h"
#include "EPD_config.h"
#include "GUI_Paint.h"
#include "ImageData.h"
#include "cJSON.h" // For parsing event JSON
#include "calendar.h"
#include "esp_log.h"
#include "font_task.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Semaphore to protect access to the E-Paper display. */
SemaphoreHandle_t xScreen = NULL;

/** @brief Frame buffer for the E-Paper display. */
UBYTE *BlackImage;

/** @brief Task handle for the main UI display task (viewDisplay). */
TaskHandle_t xViewDisplayHandle = NULL;

/** @brief Queue for receiving UI update events from other tasks. */
QueueHandle_t gui_queue;

/** @brief Log tag for this module. */
static const char *TAG = "UI_TASK";

/** @brief Static buffer to store the QR code data for user settings. */
static char setting_qrcode[256];

/** @brief X coordinate for the day number in the calendar view. */
#define CALENDAR_DAY_X 10
/** @brief Y coordinate for the day number in the calendar view. */
#define CALENDAR_DAY_Y 10
/** @brief X coordinate for the month abbreviation in the calendar view. */
#define CALENDAR_MONTH_X 10
/** @brief Y coordinate for the month abbreviation in the calendar view. */
#define CALENDAR_MONTH_Y (CALENDAR_DAY_Y + 36) // Font24 height + spacing

/** @brief X coordinate for the start of the event list in the calendar view. */
#define CALENDAR_EVENT_LIST_X (CALENDAR_DAY_X + 36 + 10)
/** @brief Y coordinate for the start of the event list in the calendar view. */
#define CALENDAR_EVENT_LIST_Y (CALENDAR_DAY_Y - 5)
/** @brief Width of the event list area in the calendar view. */
#define CALENDAR_EVENT_LIST_WIDTH (EPD_2IN9_V2_HEIGHT - CALENDAR_EVENT_LIST_X - 5)
/** @brief Height of the event list area in the calendar view. */
#define CALENDAR_EVENT_LIST_HEIGHT (EPD_2IN9_V2_WIDTH - 5) // 5px bottom padding
/** @brief Vertical spacing between lines in the event list. */
#define CALENDAR_EVENT_LINE_SPACING 5

/** @brief Font used for displaying event summaries in the calendar view. */
static sFONT *calendar_event_font = &Font16; // Font for event summaries

/**
 * @brief Sets the QR code data to be displayed.
 *
 * This function copies the provided QR code string into a static buffer.
 * The QR code is later used by the UI task to render it on the screen.
 *
 * @param qrcode A pointer to the null-terminated string containing the QR code data.
 */
void setting_qrcode_setting(char *qrcode) {
    if (qrcode != NULL && strlen(qrcode) < sizeof(setting_qrcode)) {
        memcpy(setting_qrcode, qrcode, sizeof(setting_qrcode) - 1);
        setting_qrcode[sizeof(setting_qrcode) - 1] =
            '\0'; // Ensure the string is null-terminated
    } else {
        ESP_LOGE(TAG, "Invalid QR code string");
    }
}

/**
 * @brief Formats a "YYYY-MM-DD" date string into a displayable day and month abbreviation.
 *
 * This is a helper function that takes a date string and populates two output buffers:
 * one with the two-digit day (e.g., "05") and another with the three-letter month
 * abbreviation (e.g., "Jan"). It handles invalid or "NoDate" inputs gracefully by
 * providing placeholder strings.
 *
 * @param yyyymmdd_str    Input date string in "YYYY-MM-DD" format.
 * @param out_day_str     Output buffer for the day string (e.g., "DD").
 * @param day_str_size    Size of the `out_day_str` buffer.
 * @param out_month_abbr  Output buffer for the month abbreviation (e.g., "Mon").
 * @param month_abbr_size Size of the `out_month_abbr` buffer.
 */
static void format_date_for_display(const char *yyyymmdd_str, char *out_day_str,
                                    size_t day_str_size, char *out_month_abbr,
                                    size_t month_abbr_size) {
    if (!yyyymmdd_str || strlen(yyyymmdd_str) != 10 || strcmp(yyyymmdd_str, "NoDate") == 0) {
        strncpy(out_day_str, "??", day_str_size - 1);
        out_day_str[day_str_size - 1] = '\0';
        strncpy(out_month_abbr, "???", month_abbr_size - 1);
        out_month_abbr[month_abbr_size - 1] = '\0';
        return;
    }

    strncpy(out_day_str, yyyymmdd_str + 8, 2); // Extract DD
    out_day_str[2] = '\0';

    char month_num_str[3];
    strncpy(month_num_str, yyyymmdd_str + 5, 2); // Extract MM
    month_num_str[2] = '\0';
    int month_num = atoi(month_num_str);

    const char *month_abbrs[] = {"",    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    strncpy(out_month_abbr, (month_num >= 1 && month_num <= 12) ? month_abbrs[month_num] : "???",
            month_abbr_size - 1);
    out_month_abbr[month_abbr_size - 1] = '\0';
}

/**
 * @brief The main UI task responsible for updating the E-Paper display.
 *
 * This task runs in an infinite loop, waiting for events on the `gui_queue`.
 * When an event is received, it takes the `xScreen` semaphore and updates the
 * display according to the event ID. It handles rendering different screens
 * such as Wi-Fi setup, connection status, calendar view, and QR codes.
 * It also includes logic to prevent unnecessary redraws if the view or data
 * has not changed.
 *
 * @param PvParameters Unused.
 */
void viewDisplay(void *PvParameters) {
    event_t event;
    uint32_t view_current = 0;
    char displayStr[MAX_MSG_LEN] = "";
    for (;;) {
        if (xQueueReceive(gui_queue, &event, portMAX_DELAY)) {
            ESP_LOGI("UI_TASK", "Received event: %ld", event.event_id);
            switch (event.event_id) {
            case SCREEN_EVENT_WIFI_REQUIRED:
                if ((view_current != SCREEN_EVENT_WIFI_REQUIRED) &&
                    (xSemaphoreTake(xScreen, portMAX_DELAY) == pdTRUE)) {
                    strncpy(displayStr, "Scan QR code to setup Wi-Fi", sizeof(displayStr) - 1);
                    EPD_2IN9_V2_Init_Fast();
                    EPD_2IN9_V2_Clear();
                    Paint_SelectImage(BlackImage);
                    Paint_Clear(WHITE);
                    Paint_DrawBitMap_Paste(gImage_wifiqrcode, 14, 14, 99, 99, 1);
                    Paint_DrawString_EN_Center(130, 0, 166, 70, displayStr, &Font16, WHITE, BLACK,
                                               5);
                    Paint_DrawString_EN_Center(130, 70, 166, 58, "Continue without WiFi", &Font12,
                                               BLACK, WHITE, 0);
                    Paint_DrawBitMap_Paste(gImage_arrow, 128, 93, 12, 12, 1);
                    EPD_2IN9_V2_Display(BlackImage);

                    printf("Goto Sleep...\r\n");
                    EPD_2IN9_V2_Sleep();
                    vTaskDelay(2000 / portTICK_PERIOD_MS);
                    printf("close 5V, Module enters 0 power consumption ...\r\n");
                    view_current = event.event_id;
                    xSemaphoreGive(xScreen);
                }
                break;
            case SCREEN_EVENT_NO_CONNECTION:
                if ((view_current != SCREEN_EVENT_NO_CONNECTION) &&
                    (xSemaphoreTake(xScreen, portMAX_DELAY) == pdTRUE)) {
                    strncpy(displayStr, "No server connection, retrying...",
                            sizeof(displayStr) - 1);
                    EPD_2IN9_V2_Init_Fast();
                    EPD_2IN9_V2_Clear();
                    Paint_SelectImage(BlackImage);
                    Paint_Clear(WHITE);
                    Paint_DrawString_EN_Center(0, 0, EPD_2IN9_V2_HEIGHT, EPD_2IN9_V2_WIDTH,
                                               displayStr, &Font16, WHITE, BLACK, 5);
                    Paint_DrawString_EN_Center(0, 81, EPD_2IN9_V2_HEIGHT, 47, "Wifi setting",
                                               &Font12, BLACK, WHITE, 5);
                    Paint_DrawBitMap_Paste(gImage_arrow, 90, 98, 12, 12, 1);
                    EPD_2IN9_V2_Display(BlackImage);
                    view_current = event.event_id;
                    EPD_2IN9_V2_Sleep();
                    vTaskDelay(2000 / portTICK_PERIOD_MS);
                    xSemaphoreGive(xScreen);
                }
                break;
            case SCREEN_EVENT_CENTER:
                if (strcmp(displayStr, event.msg) &&
                    (xSemaphoreTake(xScreen, portMAX_DELAY) == pdTRUE)) {
                    strncpy(displayStr, event.msg, sizeof(displayStr) - 1);
                    displayStr[sizeof(displayStr) - 1] = '\0';
                    EPD_2IN9_V2_Init_Fast();
                    // EPD_2IN9_V2_Clear();
                    Paint_SelectImage(BlackImage);
                    Paint_Clear(WHITE);
                    Paint_DrawString_EN_Center(0, 0, EPD_2IN9_V2_HEIGHT, EPD_2IN9_V2_WIDTH,
                                               displayStr, &Font16, WHITE, BLACK, 5);
                    EPD_2IN9_V2_Display(BlackImage);
                    view_current = event.event_id;
                    EPD_2IN9_V2_Sleep();
                    vTaskDelay(2000 / portTICK_PERIOD_MS);
                    xSemaphoreGive(xScreen);
                }
                break;
            case SCREEN_EVENT_CLEAR:
                if (xSemaphoreTake(xScreen, portMAX_DELAY) == pdTRUE) {
                    displayStr[0] = '\0'; // Clear the display string cache
                    EPD_2IN9_V2_Init();
                    EPD_2IN9_V2_Clear();
                    Paint_SelectImage(BlackImage);
                    Paint_Clear(WHITE);
                    EPD_2IN9_V2_Display(BlackImage);
                    view_current = event.event_id;
                    EPD_2IN9_V2_Sleep();
                    vTaskDelay(2000 / portTICK_PERIOD_MS);
                    xSemaphoreGive(xScreen);
                }
                break;
            case SCREEN_EVENT_CALENDAR:
                // If the view is already the calendar and the requested date is the same as the
                // one currently displayed, skip the redraw to prevent flickering.
                if (view_current == SCREEN_EVENT_CALENDAR && strcmp(displayStr, event.msg) == 0) {
                    ESP_LOGI(TAG, "Calendar date %s same as current, skipping full redraw.",
                             event.msg);
                    break; // Date hasn't changed, so no need to redraw.
                }

                // A redraw is needed because the view or the date has changed.
                if (xSemaphoreTake(xScreen, portMAX_DELAY) == pdTRUE) {
                    // Cache the new date string. This is used to check for changes on subsequent
                    // events.
                    strncpy(displayStr, event.msg, sizeof(displayStr) - 1);
                    displayStr[sizeof(displayStr) - 1] = '\0';

                    char disp_day[3];
                    char disp_month[4];
                    format_date_for_display(displayStr, disp_day, sizeof(disp_day), disp_month,
                                            sizeof(disp_month));

                    EPD_2IN9_V2_Init_Fast();
                    Paint_SelectImage(BlackImage);
                    Paint_Clear(WHITE);

                    // 1. Draw the date part of the UI (day and month).
                    Paint_DrawString_EN(CALENDAR_DAY_X, CALENDAR_DAY_Y, disp_day, &Font36, WHITE,
                                        BLACK);
                    Paint_DrawString_EN(CALENDAR_MONTH_X, CALENDAR_MONTH_Y, disp_month, &Font16,
                                        BLACK, WHITE);
                    Paint_DrawRectangle(5, 5, 51, 69, BLACK, 1, DRAW_FILL_EMPTY);

                    // 2. Load and draw events for the selected date from LittleFS.
                    if (strcmp(displayStr, "NoDate") != 0 && strlen(displayStr) == 10) {
                        char file_path[64];
                        int calendar_dir_len = strlen(CALENDAR_DIR);
                        memcpy(file_path, CALENDAR_DIR, calendar_dir_len);
                        file_path[calendar_dir_len] = '/';
                        memcpy(file_path + calendar_dir_len + 1, displayStr, 10);
                        memcpy(file_path + calendar_dir_len + 11, ".json", 5);
                        file_path[calendar_dir_len + 16] = '\0';

                        ESP_LOGI(TAG, "Reading calendar events from: %s", file_path);

                        FILE *f = fopen(file_path, "rb");
                        if (f) {
                            fseek(f, 0, SEEK_END);
                            long fsize = ftell(f);
                            fseek(f, 0, SEEK_SET);

                            if (fsize > 0 && fsize < 8192) { // Max file size sanity check
                                char *json_string = malloc(fsize + 1);
                                if (json_string) {
                                    size_t read_len = fread(json_string, 1, fsize, f);
                                    if (read_len == fsize) {
                                        json_string[fsize] = '\0';
                                        cJSON *root = cJSON_Parse(json_string);
                                        if (root && cJSON_IsArray(root)) {
                                            UWORD current_y = CALENDAR_EVENT_LIST_Y;
                                            UWORD line_height = calendar_event_font->Height +
                                                                CALENDAR_EVENT_LINE_SPACING;
                                            int events_displayed_count = 0;
                                            cJSON *event_item_json;

                                            cJSON_ArrayForEach(event_item_json, root) {
                                                if (current_y + calendar_event_font->Height >
                                                    CALENDAR_EVENT_LIST_Y +
                                                        CALENDAR_EVENT_LIST_HEIGHT) {
                                                    ESP_LOGI(TAG, "Event list area full.");
                                                    break; // No more space
                                                }
                                                cJSON *start = cJSON_GetObjectItemCaseSensitive(
                                                    event_item_json, "start");
                                                if (cJSON_IsString(start) &&
                                                    (start->valuestring != NULL) &&
                                                    strncmp(event.msg, start->valuestring, 10) ==
                                                        0) {

                                                    char time[6] = {0};
                                                    strncpy(time, start->valuestring + 11, 5);
                                                    Paint_DrawString_EN(
                                                        CALENDAR_EVENT_LIST_X, current_y, time,
                                                        calendar_event_font, WHITE, BLACK);
                                                }
                                                cJSON *summary = cJSON_GetObjectItemCaseSensitive(
                                                    event_item_json, "summary");
                                                if (cJSON_IsString(summary) &&
                                                    (summary->valuestring != NULL)) {
                                                    current_y +=
                                                        (line_height *
                                                         Paint_DrawString_Gen(
                                                             CALENDAR_EVENT_LIST_X + 48, current_y,
                                                             CALENDAR_EVENT_LIST_WIDTH - 48,
                                                             CALENDAR_EVENT_LIST_Y +
                                                                 CALENDAR_EVENT_LIST_HEIGHT -
                                                                 current_y,
                                                             summary->valuestring,
                                                             calendar_event_font, BLACK, WHITE));
                                                    events_displayed_count++;
                                                }
                                            }
                                            if (events_displayed_count == 0 &&
                                                cJSON_GetArraySize(root) > 0) {
                                                Paint_DrawString_Gen(
                                                    CALENDAR_EVENT_LIST_X, CALENDAR_EVENT_LIST_Y,
                                                    CALENDAR_EVENT_LIST_WIDTH,
                                                    calendar_event_font->Height,
                                                    "Events found, error displaying.",
                                                    calendar_event_font, BLACK, WHITE);
                                            } else if (cJSON_GetArraySize(root) == 0) {
                                                Paint_DrawString_Gen(
                                                    CALENDAR_EVENT_LIST_X, CALENDAR_EVENT_LIST_Y,
                                                    CALENDAR_EVENT_LIST_WIDTH,
                                                    calendar_event_font->Height,
                                                    "No events scheduled.", calendar_event_font,
                                                    BLACK, WHITE);
                                            }
                                        } else {
                                            ESP_LOGE(TAG,
                                                     "Failed to parse JSON or not an array: %s",
                                                     file_path);
                                            Paint_DrawString_Gen(
                                                CALENDAR_EVENT_LIST_X, CALENDAR_EVENT_LIST_Y,
                                                CALENDAR_EVENT_LIST_WIDTH,
                                                calendar_event_font->Height, "Event data error.",
                                                calendar_event_font, BLACK, WHITE);
                                        }
                                        if (root)
                                            cJSON_Delete(root);
                                    } else {
                                        ESP_LOGE(TAG, "Failed to read full file: %s", file_path);
                                    }
                                    free(json_string);
                                } else {
                                    ESP_LOGE(TAG, "Malloc failed for event JSON string (size %ld)",
                                             fsize);
                                }
                            } else if (fsize == 0) {
                                Paint_DrawString_Gen(
                                    CALENDAR_EVENT_LIST_X, CALENDAR_EVENT_LIST_Y,
                                    CALENDAR_EVENT_LIST_WIDTH, calendar_event_font->Height,
                                    "No events scheduled.", calendar_event_font, BLACK, WHITE);
                            }
                            fclose(f);
                        } else {
                            ESP_LOGI(TAG, "No event file found: %s", file_path);
                            Paint_DrawString_Gen(
                                CALENDAR_EVENT_LIST_X, CALENDAR_EVENT_LIST_Y,
                                CALENDAR_EVENT_LIST_WIDTH, calendar_event_font->Height,
                                "No events for this day.", calendar_event_font, BLACK, WHITE);
                        }
                    } else {
                        Paint_DrawString_Gen(CALENDAR_EVENT_LIST_X, CALENDAR_EVENT_LIST_Y,
                                             CALENDAR_EVENT_LIST_WIDTH, calendar_event_font->Height,
                                             "Date not available.", calendar_event_font, BLACK,
                                             WHITE);
                    }

                    EPD_2IN9_V2_Display(BlackImage);
                    view_current = event.event_id;
                    xSemaphoreGive(xScreen);
                }
                break;
            case SCREEN_EVENT_QRCODE:
                if ((view_current != SCREEN_EVENT_QRCODE) &&
                    (xSemaphoreTake(xScreen, portMAX_DELAY) == pdTRUE)) {
                    strncpy(displayStr, "Scan QR code to enter user settings",
                            sizeof(displayStr) - 1);
                    EPD_2IN9_V2_Init_Fast();
                    Paint_SelectImage(BlackImage);
                    Paint_Clear(WHITE);
                    Paint_DrawBitMap_Paste_Scale((UBYTE *)setting_qrcode, 14, 9, 37, 37, 0, 3);
                    Paint_DrawString_EN_Center(130, 0, 166, 70, displayStr, &Font16, WHITE, BLACK,
                                               5);
                    Paint_DrawString_EN_Center(130, 70, 166, 58, "Done", &Font12, BLACK, WHITE, 0);
                    Paint_DrawBitMap_Paste(gImage_arrow, 181, 93, 12, 12, 1);
                    EPD_2IN9_V2_Display(BlackImage);
                    view_current = event.event_id;
                    EPD_2IN9_V2_Sleep();
                    vTaskDelay(2000 / portTICK_PERIOD_MS);
                    xSemaphoreGive(xScreen);
                }
                break;
            default:
                break;
            }
        }
    }
}

/**
 * @brief Initializes the E-Paper display and starts the UI task.
 *
 * This function should be called once at startup. It initializes the display hardware,
 * allocates memory for the frame buffer, and creates the `viewDisplay` task
 * which handles all subsequent screen updates. It handles both initial boot
 * and wakeup from deep sleep scenarios. After setup is complete, it gives the
 * `xScreen` semaphore and deletes itself.
 *
 * @param pvParameters Unused.
 */
void screenStartup(void *pvParameters) {
    if (DEV_Module_Init() != 0) {
        // TODO: Handle display initialization error.
    }
    printf("e-Paper Init and Clear...\r\n");

    for (uint8_t i = 0; i < 10; i++) {
        if (xScreen == NULL) {
            printf("Failed to create semaphore for screen, retry...\r\n");
        } else {
            break;
        }
    }
    if (!isr_woken) {
        EPD_2IN9_V2_Init();
        vTaskDelay(10);
        EPD_2IN9_V2_Clear();
    }

    // Create a new image cache
    UWORD Imagesize =
        ((EPD_2IN9_V2_WIDTH % 8 == 0) ? (EPD_2IN9_V2_WIDTH / 8) : (EPD_2IN9_V2_WIDTH / 8 + 1)) *
        EPD_2IN9_V2_HEIGHT;
    if ((BlackImage = (UBYTE *)malloc(Imagesize)) == NULL) {
        printf("Failed to apply for black memory...\r\n");
    }

    Paint_NewImage(BlackImage, EPD_2IN9_V2_WIDTH, EPD_2IN9_V2_HEIGHT, 90, WHITE);
    Paint_SelectImage(BlackImage);
    Paint_Clear(WHITE);
    xTaskCreate(viewDisplay, "viewDisplay", 4096, NULL, 6, &xViewDisplayHandle);
    xSemaphoreGive(xScreen);
    vTaskDelete(NULL);
}