#include "ui_task.h"
#include "EC11_driver.h"
#include "EPD_2in9.h"
#include "EPD_config.h"
#include "GUI_Paint.h"
#include "ImageData.h"
#include "calendar.h"
#include "esp_log.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 定義螢幕用的binary semaphore
SemaphoreHandle_t xScreen = NULL;

// 定義螢幕快取
UBYTE *BlackImage;

// 定義task handle
TaskHandle_t xViewDisplayHandle = NULL;

// 定義螢幕事件
QueueHandle_t gui_queue;

static const char *TAG = "UI_TASK";

static char setting_qrcode[256];

void setting_qrcode_setting(char *qrcode) {
    if (qrcode != NULL && strlen(qrcode) < sizeof(setting_qrcode)) {
        memcpy(setting_qrcode, qrcode, sizeof(setting_qrcode) - 1);
        setting_qrcode[sizeof(setting_qrcode) - 1] = '\0'; // 確保字串結尾
    } else {
        ESP_LOGE(TAG, "Invalid QR code string");
    }
}

void viewDisplay(void *PvParameters) {
    // 等待task被呼叫
    vTaskSuspend(NULL);
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
                    displayStr[0] = '\0'; // 清空顯示字串
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
                if ((view_current != SCREEN_EVENT_CALENDAR) &&
                    (xSemaphoreTake(xScreen, portMAX_DELAY) == pdTRUE)) {
                    displayStr[0] = '\0'; // 清空顯示字串
                    EPD_2IN9_V2_Init_Fast();
                    EPD_2IN9_V2_Clear();
                    Paint_SelectImage(BlackImage);
                    Paint_Clear(WHITE);
                    Paint_DrawString_EN(0, 0, day, &Font24, BLACK, WHITE);
                    Paint_DrawString_EN(0, 24, month, &Font16, BLACK, WHITE);
                    EPD_2IN9_V2_Display(BlackImage);
                    view_current = event.event_id;
                    EPD_2IN9_V2_Sleep();
                    vTaskDelay(2000 / portTICK_PERIOD_MS);
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
                    Paint_DrawBitMap_Paste(gImage_arrow, 191, 93, 12, 12, 1);
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

void screenStartup(void *pvParameters) {
    if (DEV_Module_Init() != 0) {
    }
    printf("e-Paper Init and Clear...\r\n");

    for (uint8_t i = 0; i < 10; i++) {
        if (xScreen == NULL) {
            printf("Failed to create semaphore for screen, retry...\r\n");
        } else {
            break;
        }
    }
    EPD_2IN9_V2_Init();
    EPD_2IN9_V2_Clear();

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
    xSemaphoreGive(xScreen);
    xTaskCreate(viewDisplay, "viewDisplay", 4096, NULL, 5, &xViewDisplayHandle);
    vTaskResume(xViewDisplayHandle);
    vTaskDelete(NULL);
}