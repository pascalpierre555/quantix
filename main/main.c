/*
Copyright (c) 2017-2019 Tony Pottier

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

@file main.c
@author Tony Pottier
@brief Entry point for the ESP32 application.
@see https://idyl.io
@see https://github.com/tonyp7/esp32-wifi-manager
*/

#include <esp_netif.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "EPD_2in9.h"
#include "EPD_config.h"
#include "GUI_Paint.h"
#include "ImageData.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.h"

/* @brief tag used for ESP serial console messages */
static const char TAG[] = "main";
SemaphoreHandle_t xScreen;

void screenStartup(void *pvParameters) {
    if (DEV_Module_Init() != 0) {
    }

    printf("e-Paper Init and Clear...\r\n");
    EPD_2IN9_V2_Init();
    for (uint8_t i = 0; i < 10; i++) {
        xScreen = xSemaphoreCreateBinary();
        if (xScreen == NULL) {
            printf("Failed to create semaphore for screen, retry...\r\n");
        } else {
            break;
        }
    }

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
    char instructionText[] = "Scan QR code to setup Wi-Fi";
    Paint_DrawBitMap_Paste(gImage_wifiqrcode, 14, 14, 99, 99, 1);
    Paint_DrawString_EN(130, 10, instructionText, &Font20, WHITE, BLACK);
    EPD_2IN9_V2_Display(BlackImage);
    DEV_Delay_ms(3000);
    printf("Goto Sleep...\r\n");
    EPD_2IN9_V2_Sleep();
    DEV_Delay_ms(2000); // important, at least 2s
    // close 5V
    printf("close 5V, Module enters 0 power consumption ...\r\n");
    vTaskDelete(NULL);
}

/**
 * @brief this is an exemple of a callback that you can setup in your own app to
 * get notified of wifi manager event.
 */
void cb_connection_ok(void *pvParameter) {
    ip_event_got_ip_t *param = (ip_event_got_ip_t *)pvParameter;

    /* transform IP to human readable string */
    char str_ip[16];
    esp_ip4addr_ntoa(&param->ip_info.ip, str_ip, IP4ADDR_STRLEN_MAX);

    ESP_LOGI(TAG, "I have a connection and my IP is %s!", str_ip);
}

void app_main() {
    xTaskCreate(screenStartup, "screenStartup", 4096, NULL, 5, NULL);

    // 啟動 Wi-Fi Manager
    wifi_manager_start();

    // 設定回呼
    wifi_manager_set_callback(WM_EVENT_STA_GOT_IP, &cb_connection_ok);
}