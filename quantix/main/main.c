#include "EC11_driver.h"
#include "calendar.h"
#include "esp_err.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "sleep_manager.h"
#include "font_task.h"
#include "net_task.h"
#include "nvs_flash.h"
#include "ui_task.h"
#include "wifi_manager.h"
#include <errno.h> // For errno
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h> // For EventGroupHandle_t
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h> // For mkdir and stat

static const char *TAG_MAIN = "APP_MAIN";

void app_main() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS 分區已滿或版本不符時，執行 erase 再 init
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG_MAIN, "Initializing LittleFS");
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/littlefs",       // LittleFS 掛載點
        .partition_label = "storage",   // 對應 partitions.csv 中的標籤
        .format_if_mount_failed = true, // 如果掛載失敗則格式化
        .dont_mount = false,
    };

    // 初始化並掛載 LittleFS
    esp_err_t ret_fs = esp_vfs_littlefs_register(&conf);

    if (ret_fs != ESP_OK) {
        if (ret_fs == ESP_FAIL) {
            ESP_LOGE(TAG_MAIN, "Failed to mount or format LittleFS filesystem");
        } else if (ret_fs == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG_MAIN, "Failed to find LittleFS partition");
        } else {
            ESP_LOGE(TAG_MAIN, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret_fs));
        }
        // 根據您的應用需求處理錯誤，例如中止程式或限制功能
    } else {
        ESP_LOGI("APP_MAIN", "LittleFS mounted on %s", conf.base_path);
        // 檢查並創建字型目錄
        struct stat st = {0};
        if (stat(FONT_DIR, &st) == -1) {
            ESP_LOGI(TAG_MAIN, "Font directory %s not found, creating...", FONT_DIR);
            if (mkdir(FONT_DIR, 0755) != 0) {
                ESP_LOGE(TAG_MAIN, "Failed to create font directory %s: %s", FONT_DIR,
                         strerror(errno));
                // 根據您的應用需求處理錯誤
            } else {
                ESP_LOGI(TAG_MAIN, "Font directory %s created successfully.", FONT_DIR);
            }
        } else {
            ESP_LOGI(TAG_MAIN, "Font directory %s already exists.", FONT_DIR);
        }
        // 檢查並創建日曆目錄
        if (stat(CALENDAR_DIR, &st) == -1) {
            ESP_LOGI(TAG_MAIN, "Calendar directory %s not found, creating...", CALENDAR_DIR);
            if (mkdir(CALENDAR_DIR, 0755) != 0) {
                ESP_LOGE(TAG_MAIN, "Failed to create calendar directory %s: %s", CALENDAR_DIR,
                         strerror(errno));
            } else {
                ESP_LOGI(TAG_MAIN, "Calendar directory %s created successfully.", CALENDAR_DIR);
            }
        }
    }

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
    xWifi = xSemaphoreCreateBinary();
    if (xWifi != NULL) {
        printf("Semaphore for wifi created successfully.\r\n");
    } else {
        printf("Failed to create semaphore for wifi.\r\n");
    }

    // 創建睡眠管理事件組
    sleep_event_group = xEventGroupCreate();
    if (sleep_event_group == NULL) {
        ESP_LOGE(TAG_MAIN, "Failed to create sleep_event_group!");
        // 處理錯誤，可能中止
    }
    net_event_group = xEventGroupCreate();
    if (net_event_group == NULL) {
        ESP_LOGE(TAG_MAIN, "Failed to create net_event_group!");
        // 處理錯誤，可能中止
    }

    wakeup_handler();
    xTaskCreate(netStartup, "netStartup", 4096, NULL, 5, NULL);
    xTaskCreate(calendar_prefetch_task, "calendar_prefetch_task", 4096, NULL, 5,
                &xCalendarPrefetchHandle);
    // 創建低優先順序的睡眠管理任務
    xTaskCreate(deep_sleep_manager_task, "deep_sleep_mgr", 4096, NULL, 2, NULL);
}