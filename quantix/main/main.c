#include "EC11_driver.h"
#include "calendar.h"
#include "esp_err.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "font_task.h"
#include "net_task.h"
#include "nvs_flash.h"
#include "ui_task.h"
#include "wifi_manager.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>

void app_main() {

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS 分區已滿或版本不符時，執行 erase 再 init
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    gui_queue = xQueueCreate(EVENT_QUEUE_LENGTH, EVENT_QUEUE_ITEM_SIZE);
    if (gui_queue == NULL) {
        printf("Failed to create event_queue!\r\n");
    }

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
    xTaskCreate(ec11Startup, "ec11Startup", 4096, NULL, 5, NULL);
    xTaskCreate(ntpStartup, "ntpStartup", 4096, NULL, 5, NULL);
    xTaskCreate(calendarStartup, "calendarStartup", 4096, NULL, 5, &xcalendarStartupHandle);
    ESP_LOGI("APP_MAIN", "All tasks created");
}