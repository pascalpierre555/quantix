#include "ui_task.h"
#include "EPD_2in9.h"
#include "EPD_config.h"
#include "GUI_Paint.h"
#include "ImageData.h"
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

void viewDisplay(void *PvParameters) {
    // 等待task被呼叫
    vTaskSuspend(NULL);
    uint32_t event;
    uint32_t view_current = 0;
    for (;;) {
        event = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        switch (event) {
        case SCREEN_EVENT_WIFI_REQUIRED:
            if ((view_current != SCREEN_EVENT_WIFI_REQUIRED) &&
                (xSemaphoreTake(xScreen, portMAX_DELAY) == pdTRUE)) {
                EPD_2IN9_V2_Init_Fast();
                EPD_2IN9_V2_Clear();
                Paint_SelectImage(BlackImage);
                Paint_Clear(WHITE);

                Paint_DrawBitMap_Paste(gImage_wifiqrcode, 14, 14, 99, 99, 1);
                Paint_DrawRectangle(125, 25, 135 + Font16.Width * 14, 35 + Font16.Height * 2, BLACK,
                                    DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
                Paint_DrawString_EN(130, 30, " Scan QR code  to setup Wi-Fi", &Font16, WHITE,
                                    BLACK);
                EPD_2IN9_V2_Display(BlackImage);

                printf("Goto Sleep...\r\n");
                EPD_2IN9_V2_Sleep();
                DEV_Delay_ms(2000);
                printf("close 5V, Module enters 0 power consumption ...\r\n");
                view_current = event;
                xSemaphoreGive(xScreen);
            }
            break;
        default:
            break;
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
    vTaskDelete(NULL);
}