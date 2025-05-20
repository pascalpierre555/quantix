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

SemaphoreHandle_t xScreen = NULL;

void screenStartup(void *pvParameters) {
    if (DEV_Module_Init() != 0) {
    }
    printf("e-Paper Init and Clear...\r\n");

    for (uint8_t i = 0; i < 10; i++) {
        xScreen = xSemaphoreCreateBinary();
        if (xScreen == NULL) {
            printf("Failed to create semaphore for screen, retry...\r\n");
        } else {
            break;
        }
    }
    if (xSemaphoreTake(xScreen, 0) == pdTRUE) {
        printf("Semaphore for screen created successfully.\r\n");
    } else {
        printf("Failed to create semaphore for screen.\r\n");
    }
    EPD_2IN9_V2_Init();
    EPD_2IN9_V2_Clear();

    // Create a new image cache
    UBYTE *BlackImage;
    UWORD Imagesize =
        ((EPD_2IN9_V2_WIDTH % 8 == 0) ? (EPD_2IN9_V2_WIDTH / 8)
                                      : (EPD_2IN9_V2_WIDTH / 8 + 1)) *
        EPD_2IN9_V2_HEIGHT;
    if ((BlackImage = (UBYTE *)malloc(Imagesize)) == NULL) {
        printf("Failed to apply for black memory...\r\n");
    }
    Paint_NewImage(BlackImage, EPD_2IN9_V2_WIDTH, EPD_2IN9_V2_HEIGHT, 90,
                   WHITE);
    Paint_SelectImage(BlackImage);
    DEV_Delay_ms(1000);
    Paint_Clear(WHITE);

    // Draw qrcode and instruction text
    char instructionText[] = " Scan QR code  to setup Wi-Fi";
    Paint_DrawBitMap_Paste(gImage_wifiqrcode, 14, 14, 99, 99, 1);
    Paint_DrawString_EN(130, 30, instructionText, &Font16, WHITE, BLACK);
    EPD_2IN9_V2_Display(BlackImage);
    DEV_Delay_ms(3000);

    EPD_2IN9_V2_Init();
    EPD_2IN9_V2_Clear();
    printf("Goto Sleep...\r\n");
    EPD_2IN9_V2_Sleep();
    DEV_Delay_ms(2000); // important, at least 2s
    // close 5V
    printf("close 5V, Module enters 0 power consumption ...\r\n");
    xSemaphoreGive(xScreen);
    vTaskDelete(NULL);
}
