#include "esp_err.h"
#include "esp_log.h"
#include "net_task.h"
#include "nvs_flash.h"
#include "ui_task.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>

void app_main() {
    // 初始化 NVS
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
    xTaskCreate(viewDisplay, "viewDisplay", 4096, NULL, 5, &xViewDisplayHandle);
    vTaskResume(xViewDisplayHandle);
    ESP_LOGI("APP_MAIN", "viewDisplay task created");
    xTaskCreate(statusCheck, "statusCheck", 4096, NULL, 2, &xStatusCheckHandle);
    xTaskNotifyGive(xStatusCheckHandle);
    ESP_LOGI("APP_MAIN", "All tasks created");
}