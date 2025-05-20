#include "net_task.h"
#include "ui_task.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>

void app_main() {
    xTaskCreate(screenStartup, "screenStartup", 4096, NULL, 5, NULL);

    // // 啟動 Wi-Fi Manager
    // wifi_manager_start();

    // // 設定回呼
    // wifi_manager_set_callback(WM_EVENT_STA_GOT_IP, &cb_connection_ok);
}