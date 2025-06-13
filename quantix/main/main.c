#include "EC11_driver.h"
#include "calendar.h"
#include "esp_err.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_sleep.h" // For deep sleep wakeup cause
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

RTC_DATA_ATTR bool isr_woken = false;

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

    // 檢查喚醒原因
    esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();
    switch (wakeup_cause) {
    case ESP_SLEEP_WAKEUP_EXT1: {
        uint64_t wakeup_pin_mask = esp_sleep_get_ext1_wakeup_status();
        isr_woken = true;
        ESP_LOGI(TAG_MAIN, "Woke up from deep sleep by EXT1. Wakeup pin mask: 0x%llx",
                 wakeup_pin_mask);
        xTaskCreate(screenStartup, "screenStartup", 4096, NULL, 6, NULL);
        xTaskCreate(calendar_display, "calendar_display", 4096, NULL, 6, &xCalendarDisplayHandle);
        ec11Startup();
        font_table_init();
        xEventGroupSetBits(net_event_group, NET_CALENDAR_AVAILABLE_BIT);
        if (wakeup_pin_mask & (1ULL << PIN_BUTTON)) {
            ESP_LOGI(TAG_MAIN, "Wakeup caused by PIN_BUTTON (GPIO %d)", PIN_BUTTON);
        }
        if (wakeup_pin_mask & (1ULL << PIN_ENCODER_A)) {
            ESP_LOGI(TAG_MAIN, "Wakeup caused by PIN_ENCODER_A (GPIO %d)", PIN_ENCODER_A);
            xTaskNotify(xCalendarDisplayHandle, 1, eSetValueWithOverwrite);
            ec11_set_encoder_callback(xCalendarDisplayHandle);
        }
        if (wakeup_pin_mask & (1ULL << PIN_ENCODER_B)) {
            ESP_LOGI(TAG_MAIN, "Wakeup caused by PIN_ENCODER_B (GPIO %d)", PIN_ENCODER_B);
            xTaskNotify(xCalendarDisplayHandle, 2, eSetValueWithOverwrite);
            ec11_set_encoder_callback(xCalendarDisplayHandle);
        }
        // 在此處可以根據喚醒的腳位執行特定操作
        break;
    }
    case ESP_SLEEP_WAKEUP_TIMER:
        ESP_LOGI(TAG_MAIN, "Woke up from deep sleep by timer.");
        break;
    // 其他喚醒原因，通常視為正常啟動或初次啟動
    default:
        if (wakeup_cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
            // 僅在初次啟動時創建所有主要應用任務
            // 和新的睡眠管理任務
            ESP_LOGI(TAG_MAIN, "Initial boot (power-on reset or undefined wakeup cause).");
            xTaskCreate(screenStartup, "screenStartup", 4096, NULL, 6, NULL);
            xTaskCreate(calendar_display, "calendar_display", 4096, NULL, 6,
                        &xCalendarDisplayHandle); // Create CalenderStartupNoWifi task
            ec11Startup();
            font_table_init();
            ESP_LOGI(TAG_MAIN, "All tasks created");
        } else {
            ESP_LOGI(TAG_MAIN, "Normal boot or wake up from non-deep-sleep event (cause: %d).",
                     wakeup_cause);
            // 這可能是軟體重啟、從輕度睡眠喚醒等
            // 在從深度睡眠喚醒後，我們通常需要重新初始化硬體和一些任務狀態，
            // 但核心任務的創建通常只在冷啟動時進行。
            // 這裡，我們確保睡眠管理器任務在喚醒後也運行（如果它不是在冷啟動時創建的）。
            // 但根據目前的邏輯，它只在 ESP_SLEEP_WAKEUP_UNDEFINED 時創建。
            // 如果您希望在每次喚醒時都重新啟動所有任務，則需要調整此處的邏輯。
        }
        // 任何非深度睡眠喚醒的通用啟動邏輯可以放在這裡
        break;
    }
    xTaskCreate(netStartup, "netStartup", 4096, NULL, 5, NULL);
    xTaskCreate(calendar_prefetch_task, "calendar_prefetch_task", 4096, NULL, 5,
                &xCalendarPrefetchHandle);
    // 創建低優先順序的睡眠管理任務
    xTaskCreate(deep_sleep_manager_task, "deep_sleep_mgr", 4096, NULL, 2, NULL);
}