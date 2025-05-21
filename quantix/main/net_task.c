#include "JWT_storage.h"
#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "ui_task.h"
#include "wifi_manager.h"
#include <arpa/inet.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "NET_TASK";

// 定義 HTTP 事件處理函式
esp_err_t _http_event_handler(esp_http_client_event_t *evt) { return ESP_OK; }

// 定義登入 URL 和股票 API URL
#define LOGIN_URL "https://peng-pc.tail941dce.ts.net/login"
// #define STOCK_URL "http://<your-server-ip>:5000/stock"

// 定義task handle
TaskHandle_t xStatusCheckHandle = NULL;

/**
 * @brief 檢查 HTTP 連線
 *
 * 此函式會檢查與指定 URL 的 HTTP
 * 連線是否成功。若成功，則輸出狀態碼；若失敗，則輸出錯誤訊息。
 *
 * @param void
 */

bool http_check_connectivity() {
    esp_http_client_config_t config = {
        .url = "https://peng-pc.tail941dce.ts.net/", // 換成你的 Flask API 位置
        .event_handler = _http_event_handler,
        .timeout_ms = 3000};

    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP connected successfully. Status = %d",
                 esp_http_client_get_status_code(client));
        esp_http_client_cleanup(client);
        return 1;
    } else {
        ESP_LOGE(TAG, "HTTP connection failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return 0;
    }
}

/**
 * @brief WiFi 連線成功後的回呼函式
 *
 * 此函式會在 WiFi 連線成功並取得 IP 時被呼叫，並將取得的 IP 轉為字串後輸出至
 * log。
 *
 * @param pvParameter 指向 ip_event_got_ip_t 結構的指標，包含 IP 資訊
 */

void cb_connection_ok(void *pvParameter) {
    ip_event_got_ip_t *param = (ip_event_got_ip_t *)pvParameter;

    /* transform IP to human readable string */
    char str_ip[16];
    esp_ip4addr_ntoa(&param->ip_info.ip, str_ip, IP4ADDR_STRLEN_MAX);

    ESP_LOGI(TAG, "I have a connection and my IP is %s!", str_ip);
    ESP_LOGI(TAG, "Checking HTTP connectivity...");
    if (!http_check_connectivity()) {
        ESP_LOGE(TAG, "HTTP connectivity check failed.");
        ESP_LOGI(TAG, "Switching to AP mode...");
        xTaskNotify(xViewDisplayHandle, SCREEN_EVENT_WIFI_REQUIRED, eSetValueWithoutOverwrite);
        wifi_manager_clear_sta_config();                          // 清除記憶的 SSID/password
        wifi_manager_send_message(WM_ORDER_DISCONNECT_STA, NULL); // 斷線
        wifi_manager_send_message(WM_ORDER_START_AP, NULL);       // 啟動配網模式
    }
}

void server_login(void *pvParameter) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        ESP_LOGI(TAG, "No token found. Logging in...");

        const char *post_data = "{\"username\":\"esp32\", \"password\":\"supersecret\"}";
        esp_http_client_config_t config = {
            .url = LOGIN_URL,
            .method = HTTP_METHOD_POST,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);

        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_data, strlen(post_data));

        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            int len = esp_http_client_get_content_length(client);
            char *buf = malloc(len + 1);
            esp_http_client_read(client, buf, len);
            buf[len] = '\0';

            cJSON *root = cJSON_Parse(buf);
            const char *jwt = cJSON_GetObjectItem(root, "token")->valuestring;
            jwt_save_to_nvs(jwt);

            cJSON_Delete(root);
            free(buf);
        } else {
            ESP_LOGE(TAG, "Login failed: %s", esp_err_to_name(err));
        }
        esp_http_client_cleanup(client);
    }
}

/**
 * @brief 啟動網路相關功能
 *
 * 此函式會啟動 WiFi 管理器，並設置 WiFi 連線成功後的回呼函式。
 *
 * @param void
 */

void netStartup(void *pvParameters) {
    wifi_manager_start();
    wifi_manager_set_callback(WM_EVENT_STA_GOT_IP, &cb_connection_ok);
    vTaskDelete(NULL);
}

void statusCheck(void *pvParameters) {
    TickType_t xLastWakeTime;
    const TickType_t xInterval = pdMS_TO_TICKS(60000); // 60秒

    xLastWakeTime = xTaskGetTickCount();

    for (;;) {
        // 等待 60 秒或收到通知就立即執行
        ulTaskNotifyTake(pdTRUE, xInterval);

        ESP_LOGI(TAG, "Checking HTTP connectivity...");
        if (!http_check_connectivity()) {
            ESP_LOGE(TAG, "HTTP connectivity check failed.");
            ESP_LOGI(TAG, "Switching to AP mode...");
            xTaskNotify(xViewDisplayHandle, SCREEN_EVENT_WIFI_REQUIRED, eSetValueWithoutOverwrite);
            wifi_manager_clear_sta_config();
            wifi_manager_send_message(WM_ORDER_DISCONNECT_STA, NULL);
            wifi_manager_send_message(WM_ORDER_START_AP, NULL);
        }
        xLastWakeTime = xTaskGetTickCount();
    }
}

// void app_main(void) {
//     char *token = jwt_load_from_nvs();
//     if (!token) {
//         server_login();
//     }
//
// if (token) {
//     ESP_LOGI(TAG, "Token: %s", token);
//     esp_http_client_config_t config = {
//         .url = STOCK_URL,
//         .method = HTTP_METHOD_GET,
//     };
//     esp_http_client_handle_t client = esp_http_client_init(&config);

//     char auth_header[512];
//     snprintf(auth_header, sizeof(auth_header), "Bearer %s", token);
//     esp_http_client_set_header(client, "Authorization", auth_header);

//     esp_err_t err = esp_http_client_perform(client);
//     if (err == ESP_OK) {
//         int len = esp_http_client_get_content_length(client);
//         char *buf = malloc(len + 1);
//         esp_http_client_read(client, buf, len);
//         buf[len] = '\0';

//         ESP_LOGI(TAG, "Stock result: %s", buf);
//         free(buf);
//     } else {
//         ESP_LOGE(TAG, "Stock fetch failed: %s", esp_err_to_name(err));
//     }
//     esp_http_client_cleanup(client);
// }
//     free(token);
// }