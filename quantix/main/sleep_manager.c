#include "sleep_manager.h"
#include "EC11_driver.h"
#include "esp_sleep.h" // For deep sleep wakeup cause
#include "task_handles.h"

#define TAG "SleepManager"

RTC_DATA_ATTR bool isr_woken = false;

void wakeup_handler(void) {
    // 檢查喚醒原因
    esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();
    switch (wakeup_cause) {
    case ESP_SLEEP_WAKEUP_EXT1: {
        uint64_t wakeup_pin_mask = esp_sleep_get_ext1_wakeup_status();
        isr_woken = true;
        ESP_LOGI(TAG, "Woke up from deep sleep by EXT1. Wakeup pin mask: 0x%llx", wakeup_pin_mask);
        xTaskCreate(screenStartup, "screenStartup", 4096, NULL, 6, NULL);
        xTaskCreate(calendar_display, "calendar_display", 4096, NULL, 6, &xCalendarDisplayHandle);
        ec11Startup();
        font_table_init();
        ec11_set_encoder_callback(xCalendarDisplayHandle);
        xEventGroupSetBits(net_event_group, NET_CALENDAR_AVAILABLE_BIT);
        if (wakeup_pin_mask & (1ULL << PIN_BUTTON)) {
            ESP_LOGI(TAG, "Wakeup caused by PIN_BUTTON (GPIO %d)", PIN_BUTTON);
        }
        if (wakeup_pin_mask & (1ULL << PIN_ENCODER_A)) {
            ESP_LOGI(TAG, "Wakeup caused by PIN_ENCODER_A (GPIO %d)", PIN_ENCODER_A);
            xTaskNotify(xCalendarDisplayHandle, 2, eSetValueWithOverwrite);
        }
        if (wakeup_pin_mask & (1ULL << PIN_ENCODER_B)) {
            ESP_LOGI(TAG, "Wakeup caused by PIN_ENCODER_B (GPIO %d)", PIN_ENCODER_B);
            xTaskNotify(xCalendarDisplayHandle, 1, eSetValueWithOverwrite);
        }
        break;
    }
    case ESP_SLEEP_WAKEUP_TIMER:
        ESP_LOGI(TAG, "Woke up from deep sleep by timer.");
        break;
    // 其他喚醒原因，通常視為正常啟動或初次啟動
    default:
        if (wakeup_cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
            // 僅在初次啟動時創建所有主要應用任務
            // 和新的睡眠管理任務
            ESP_LOGI(TAG, "Initial boot (power-on reset or undefined wakeup cause).");
            xTaskCreate(screenStartup, "screenStartup", 4096, NULL, 6, NULL);
            xTaskCreate(calendar_display, "calendar_display", 4096, NULL, 6,
                        &xCalendarDisplayHandle); // Create CalenderStartupNoWifi task
            ec11Startup();
            font_table_init();
            ESP_LOGI(TAG, "All tasks created");
        } else {
            ESP_LOGI(TAG, "Normal boot or wake up from non-deep-sleep event (cause: %d).",
                     wakeup_cause);
        }
        break;
    }
}