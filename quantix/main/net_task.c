#include "net_task.h"
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
#include <freertos/event_groups.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "NET_TASK";

// 定義憑證檔案的路徑
extern const char isrgrootx1_pem_start[] asm("_binary_isrgrootx1_pem_start");
extern const char isrgrootx1_pem_end[] asm("_binary_isrgrootx1_pem_end");

static char responseBuffer[512];

// 定義登入 URL 和股票 API URL
#define LOGIN_URL "https://peng-pc.tail941dce.ts.net/login"
#define TEST_URL "https://peng-pc.tail941dce.ts.net/ping"
// #define STOCK_URL "http://<your-server-ip>:5000/stock"
#define MAX_BACKOFF_MS 15 * 60 * 1000 // 最大退避時間為 15 分鐘

// 定義task handle
TaskHandle_t xServerCheckHandle = NULL;
TaskHandle_t xServerLoginHandle = NULL;

EventGroupHandle_t wifi_event_group;

// 定義 HTTP 事件處理函式
esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER: %s: %s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA: %d bytes", evt->data_len);
        if (evt->user_data) {
            memcpy(evt->user_data, evt->data, evt->data_len);
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    default:
        break;
    }
    return ESP_OK;
}

// 定義登入事件處理函式
esp_err_t login_event_handler(esp_http_client_event_t *evt) {
    static int output_len = 0; // Stores number of bytes read

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        // If user_data is set, copy the response into the buffer
        if (!esp_http_client_is_chunked_response(evt->client) &&
            esp_http_client_get_status_code(evt->client) == 200) {
            memcpy(responseBuffer + output_len, evt->data, evt->data_len);
            output_len += evt->data_len;
            responseBuffer[output_len] = '\0'; // Null-terminate the string
        }
        break;
    default:
        break;
    }

    return ESP_OK;
}

/**
 * @brief 檢查 HTTP 連線
 *
 * 此函式會檢查與指定 URL 的 HTTP
 * 連線是否成功。若成功，則輸出狀態碼；若失敗，則輸出錯誤訊息。
 *
 * @param void
 */

bool http_check_server_connectivity(esp_http_client_handle_t client) {
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

void server_login(void *pvParameter) {
    ESP_LOGI(TAG, "No token found. Logging in...");
    // char *buf = malloc((buf_size + 1) * sizeof(char));
    const char *post_data = "{\"username\":\"esp32\", \"password\":\"supersecret\"}";
    esp_http_client_config_t config = {
        .username = "esp32",
        .password = "supersecret",
        .url = LOGIN_URL,
        .event_handler = login_event_handler,
        .timeout_ms = 3000,
        .cert_pem = isrgrootx1_pem_start,
        .method = HTTP_METHOD_POST,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    TickType_t token_expire_time = 60 * 60 * 1000; // 重置 token 過期時間

    for (;;) {
        ulTaskNotifyTake(pdTRUE, token_expire_time / portTICK_PERIOD_MS); // 等待通知喚醒
        xEventGroupClearBits(wifi_event_group, TOKEN_AVAILABLE_BIT);
        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "HTTP response: %s", responseBuffer);
            cJSON *root = cJSON_Parse(responseBuffer);
            if (root) {
                cJSON *token_item = cJSON_GetObjectItem(root, "token");
                if (token_item && cJSON_IsString(token_item)) {
                    const char *jwt = token_item->valuestring;
                    jwt_save_to_nvs(jwt);
                } else {
                    ESP_LOGE(TAG, "No 'token' in JSON response!");
                }
                cJSON_Delete(root);
                token_expire_time = 60 * 60 * 1000;
                xEventGroupSetBits(wifi_event_group, TOKEN_AVAILABLE_BIT);
            } else {
                ESP_LOGE(TAG, "Failed to parse JSON: %s", responseBuffer);
            }
        } else {
            ESP_LOGE(TAG, "Login failed: %s", esp_err_to_name(err));
            xEventGroupClearBits(wifi_event_group, SERVER_CONNECTED_BIT);
            xTaskNotifyGive(xServerCheckHandle); // 喚醒 server_check_task
        }
    }
    esp_http_client_cleanup(client);
}

void cb_connection_ok(void *pvParameter) {
    ip_event_got_ip_t *param = (ip_event_got_ip_t *)pvParameter;

    /* transform IP to human readable string */
    char str_ip[16];
    esp_ip4addr_ntoa(&param->ip_info.ip, str_ip, IP4ADDR_STRLEN_MAX);

    ESP_LOGI(TAG, "I have a connection and my IP is %s!", str_ip);
    xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    event_t ev = {
        .event_id = SCREEN_EVENT_CENTER,
        .msg = "WiFi connected:)",
    };
    xQueueSend(event_queue, &ev, portMAX_DELAY);
    xTaskNotifyGive(xServerCheckHandle); // 喚醒 server_check_task
}

void cb_wifi_required(void *pvParameter) {
    ESP_LOGI(TAG, "WiFi required, switching to AP mode...");
    event_t ev = {
        .event_id = SCREEN_EVENT_WIFI_REQUIRED,
        .msg = "WiFi required, switching to AP mode",
    };
    xQueueSend(event_queue, &ev, portMAX_DELAY);
}

/**
 * @brief 啟動網路相關功能
 *
 * 此函式會啟動 WiFi 管理器，並設置 WiFi 連線成功後的回呼函式。
 *
 * @param void
 */

void server_check_task(void *pvParameters) {
    uint8_t success_count = 0;
    uint8_t failure_count = 0;
    TickType_t retry_delay_ms = 1000;

    esp_http_client_config_t config = {
        .url = LOGIN_URL,
        .event_handler = _http_event_handler,
        .timeout_ms = 3000,
        .cert_pem = isrgrootx1_pem_start,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    for (;;) {
        // Block，等待其他任務用 xTaskNotifyGive() 喚醒
        ulTaskNotifyTake(pdTRUE, retry_delay_ms / portTICK_PERIOD_MS);
        retry_delay_ms = 1000;
        xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdTRUE, pdFALSE, portMAX_DELAY);

        if (http_check_server_connectivity(client)) {
            ESP_LOGI(TAG, "Send success");
            success_count++;
            failure_count = 0;
            retry_delay_ms = 1000;
            xEventGroupSetBits(wifi_event_group, SERVER_CONNECTED_BIT);
            event_t ev = {
                .event_id = SCREEN_EVENT_CENTER,
                .msg = "Server connected successfully:)",
            };
            xQueueSend(event_queue, &ev, portMAX_DELAY);
            ESP_LOGI(TAG, "Server connected successfully, success count: %d", success_count);
            if (!(TOKEN_AVAILABLE_BIT && xEventGroupGetBits(wifi_event_group))) {
                // 如果沒有 token，則嘗試登入
                xTaskNotifyGive(xServerLoginHandle);
            }
            vTaskSuspend(NULL);
            break;
        } else {
            xEventGroupClearBits(wifi_event_group, SERVER_CONNECTED_BIT);
            failure_count++;
            ESP_LOGW(TAG, "Send failed, count: %d", failure_count);

            if (success_count >= 5) {
                retry_delay_ms = 3 * 1000;
            } else if (failure_count == 1) {
                event_t ev = {
                    .event_id = SCREEN_EVENT_CENTER,
                    .msg = "No server connection, retrying...",
                };
                xQueueSend(event_queue, &ev, portMAX_DELAY);
                retry_delay_ms = 5 * 1000;
            } else if (failure_count < 6) {
                retry_delay_ms = 5 * 1000;
            } else if (failure_count < 10) {
                retry_delay_ms = 30 * 1000;
            } else {
                retry_delay_ms = 5 * 60 * 1000;
            }

            if (retry_delay_ms > MAX_BACKOFF_MS) {
                retry_delay_ms = MAX_BACKOFF_MS;
            }

            ESP_LOGI(TAG, "Retrying after %lu ms", (unsigned long)retry_delay_ms);
        }
    }

    // TickType_t xLastWakeTime;
    // const TickType_t xInterval = pdMS_TO_TICKS(60000); // 60秒

    // xLastWakeTime = xTaskGetTickCount();

    // for (;;) {
    //     // 等待 60 秒或收到通知就立即執行
    //     ulTaskNotifyTake(pdTRUE, xInterval);

    //     ESP_LOGI(TAG, "Checking HTTP connectivity...");
    //     if (!http_check_connectivity()) {
    //         ESP_LOGE(TAG, "HTTP connectivity check failed.");
    //         ESP_LOGI(TAG, "Switching to AP mode...");
    //         xTaskNotify(xViewDisplayHandle, SCREEN_EVENT_WIFI_REQUIRED,
    //         eSetValueWithoutOverwrite); if (!wifi_manager_is_ap_started()) {
    //             wifi_manager_send_message(WM_ORDER_START_AP, NULL);
    //         }
    //     }
    //     xLastWakeTime = xTaskGetTickCount();
    // }
}

void netStartup(void *pvParameters) {
    wifi_manager_start();
    wifi_manager_set_callback(WM_EVENT_STA_GOT_IP, &cb_connection_ok);
    wifi_manager_set_callback(WM_ORDER_START_AP, &cb_wifi_required);
    xTaskCreate(server_check_task, "server_check_task", 4096, NULL, 2, &xServerCheckHandle);
    xTaskCreate(server_login, "server_login", 4096, NULL, 2, &xServerLoginHandle);
    wifi_event_group = xEventGroupCreate();
    vTaskDelete(NULL);
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