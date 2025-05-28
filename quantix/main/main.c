#include "esp_err.h"
#include "esp_log.h"
#include "net_task.h"
#include "nvs_flash.h"
#include "ui_task.h"
#include "wifi_manager.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>

void app_main() {

    ESP_ERROR_CHECK(nvs_flash_init());

    // 創建semaphore
    xScreen = xSemaphoreCreateBinary();
    if (xScreen != NULL) {
        printf("Semaphore for screen created successfully.\r\n");
    } else {
        printf("Failed to create semaphore for screen.\r\n");
    }

    // 創建所有任務
    xTaskCreate(screenStartup, "screenStartup", 4096, NULL, 5, NULL);
    xTaskCreate(netStartup, "netStartup", 4096, NULL, 5, NULL);
    ESP_LOGI("APP_MAIN", "All tasks created");
}