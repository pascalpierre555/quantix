#include "net_task.h"
#include "EC11_driver.h"
#include "EPD_2in9.h"
#include "EPD_config.h"
#include "GUI_Paint.h"
#include "JWT_storage.h"
#include "cJSON.h"
#include "calendar.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "mbedtls/base64.h"
#include "ui_task.h"
#include "wifi_manager.h"
#include <arpa/inet.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/event_groups.h>
#include <nvs.h>
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
#define SETTING_URL "https://peng-pc.tail941dce.ts.net/settings"
#define CHECK_AUTH_RESULT_URL "https://peng-pc.tail941dce.ts.net/check_auth_result?username=esp32"
// #define STOCK_URL "http://<your-server-ip>:5000/stock"
#define MAX_BACKOFF_MS 15 * 60 * 1000 // 最大退避時間為 15 分鐘

// 定義task handle
TaskHandle_t xServerCheckHandle = NULL;
TaskHandle_t xServerLoginHandle = NULL;
TaskHandle_t xUserSettingsHandle = NULL;
TaskHandle_t xEspCheckAuthResultHandle = NULL;

EventGroupHandle_t net_event_group;

static int output_len = 0; // Stores number of bytes read

// 定義 HTTP 事件處理函式
esp_err_t _http_event_handler(esp_http_client_event_t *evt) { return ESP_OK; }

// 定義登入事件處理函式
esp_err_t login_event_handler(esp_http_client_event_t *evt) {

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

bool http_response_save_to_nvs(cJSON *root, char *nvs_name, char *key) {
    if (root && key) {
        cJSON *item = cJSON_GetObjectItem(root, key);
        if (item && cJSON_IsString(item)) {
            // 儲存到 NVS
            nvs_handle_t nvs;
            esp_err_t err = nvs_open(nvs_name, NVS_READWRITE, &nvs);
            if (err == ESP_OK) {
                nvs_set_str(nvs, key, item->valuestring);
                nvs_commit(nvs);
                nvs_close(nvs);
                ESP_LOGI(TAG, "Saved %s to NVS: %s", key, item->valuestring);
            } else {
                ESP_LOGE(TAG, "Failed to open NVS for %s: %s", key, esp_err_to_name(err));
                return 1;
            }
        } else {
            ESP_LOGE(TAG, "No valid item found for key: %s", key);
            return 1;
        }
    } else {
        ESP_LOGE(TAG, "Invalid parameters: %s", key);
        return 1;
    }
    return 0;
}

void cb_button_wifi_settings(void) {
    ec11_clean_button_callback();
    wifi_manager_send_message(WM_ORDER_START_AP, NULL);
}

void cb_button_continue_without_wifi(void) {
    ESP_LOGI(TAG, "Continuing without WiFi...");
    ec11_clean_button_callback();
}

void cb_button_setting_done(void) {
    ec11_clean_button_callback();
    vTaskNotifyGiveFromISR(xEspCheckAuthResultHandle, NULL);
}

void esp_check_auth_result(void *pvParameters) {
    esp_http_client_config_t config = {
        .url = CHECK_AUTH_RESULT_URL,
        .event_handler = login_event_handler,
        .timeout_ms = 3000,
        .cert_pem = isrgrootx1_pem_start,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    event_t ev = {
        .event_id = SCREEN_EVENT_CENTER,
        .msg = "Please try again, status: ",
    };
    event_t evqr = {
        .event_id = SCREEN_EVENT_QRCODE,
    };
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // 等待通知喚醒
        char *token = jwt_load_from_nvs();
        output_len = 0; // Reset output length
        static char auth_header[256];
        if (token) {
            snprintf(auth_header, sizeof(auth_header), "Bearer %s", token);
            esp_http_client_set_header(client, "Authorization", auth_header);
            output_len = 0;
            esp_err_t err = esp_http_client_perform(client);
            if (err == ESP_OK) {
                cJSON *root = cJSON_Parse(responseBuffer);
                if (root) {
                    ESP_LOGI(TAG, "HTTP response: %s", responseBuffer);
                    if (http_response_save_to_nvs(root, "calendar", "email")) {
                        cJSON *item = cJSON_GetObjectItem(root, "status");
                        char *status = cJSON_GetStringValue(item);
                        if (status) {
                            ESP_LOGI(TAG, "Status: %s", status);
                            ev.msg[26] = '\0'; // 確保字串結尾
                            strncat(ev.msg, status, MAX_MSG_LEN - strlen(ev.msg) - 1);
                            xQueueSend(gui_queue, &ev, portMAX_DELAY);
                            xSemaphoreTake(xScreen, portMAX_DELAY);
                            ec11_set_button_callback(&cb_button_setting_done);
                            xSemaphoreGive(xScreen);
                            xQueueSend(gui_queue, &evqr, portMAX_DELAY);
                        }
                    } else {
                        http_response_save_to_nvs(root, "calendar", "access_token");
                        http_response_save_to_nvs(root, "calendar", "refresh_token");
                        xTaskNotifyGive(xcalendarStartupHandle);
                    }
                } else {
                    ESP_LOGE(TAG, "Failed to parse JSON: %s", responseBuffer);
                }
                cJSON_Delete(root);
            }
        }
    }
    esp_http_client_cleanup(client);
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
        return 1;
    } else {
        ESP_LOGE(TAG, "HTTP connection failed: %s", esp_err_to_name(err));
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
        ESP_LOGI(TAG, "No token found. Logging in...");
        xEventGroupClearBits(net_event_group, NET_TOKEN_AVAILABLE_BIT);
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
                xEventGroupSetBits(net_event_group, NET_TOKEN_AVAILABLE_BIT);
            } else {
                token_expire_time = 500;
                ESP_LOGE(TAG, "Failed to parse JSON: %s", responseBuffer);
            }
        } else {
            ESP_LOGE(TAG, "Login failed: %s", esp_err_to_name(err));
            xEventGroupClearBits(net_event_group, NET_SERVER_CONNECTED_BIT);
            vTaskResume(xServerCheckHandle);     // 喚醒 server_check_task
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
    xEventGroupSetBits(net_event_group, NET_WIFI_CONNECTED_BIT);
    event_t ev = {
        .event_id = SCREEN_EVENT_CENTER,
        .msg = "WiFi connected:)",
    };
    xQueueSend(gui_queue, &ev, portMAX_DELAY);
    xTaskNotifyGive(xServerCheckHandle); // 喚醒 server_check_task
}

void cb_wifi_required(void *pvParameter) {
    ESP_LOGI(TAG, "WiFi required, switching to AP mode...");
    event_t ev = {
        .event_id = SCREEN_EVENT_WIFI_REQUIRED,
        .msg = "WiFi required, switching to AP mode",
    };
    xQueueSend(gui_queue, &ev, portMAX_DELAY);
    ec11_set_button_callback(&cb_button_continue_without_wifi);
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
    esp_http_client_config_t config = {
        .url = TEST_URL,
        .event_handler = _http_event_handler,
        .timeout_ms = 3000,
        .cert_pem = isrgrootx1_pem_start,
    };
    esp_http_client_handle_t client;
    TickType_t retry_delay_ms = 1000;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, retry_delay_ms / portTICK_PERIOD_MS);
        xEventGroupWaitBits(net_event_group, NET_WIFI_CONNECTED_BIT, pdFALSE, pdFALSE,
                            portMAX_DELAY);

        client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "Failed to initialize HTTP client");
            vTaskDelay(retry_delay_ms / portTICK_PERIOD_MS);
            continue;
        }
        if (http_check_server_connectivity(client)) {
            ESP_LOGI(TAG, "Send success");
            success_count++;
            failure_count = 0;
            retry_delay_ms = 1000;
            if (!(NET_TOKEN_AVAILABLE_BIT & xEventGroupGetBits(net_event_group))) {
                // 如果沒有 token，則嘗試登入
                xTaskNotifyGive(xServerLoginHandle);
            }
            xEventGroupWaitBits(net_event_group, NET_TOKEN_AVAILABLE_BIT, pdFALSE, pdFALSE,
                                portMAX_DELAY);
            xEventGroupSetBits(net_event_group, NET_SERVER_CONNECTED_BIT);
            event_t ev = {
                .event_id = SCREEN_EVENT_CENTER,
                .msg = "Server connected successfully:)",
            };
            xQueueSend(gui_queue, &ev, portMAX_DELAY);
            ESP_LOGI(TAG, "Server connected successfully, success count: %d", success_count);
            xTaskNotifyGive(xcalendarStartupHandle);

            // 等待 token 可用
            vTaskSuspend(NULL);
        } else {
            xEventGroupClearBits(net_event_group, NET_SERVER_CONNECTED_BIT);
            success_count = 0;
            failure_count++;
            ESP_LOGW(TAG, "Send failed, count: %d", failure_count);

            if (failure_count == 1) {
                event_t ev = {
                    .event_id = SCREEN_EVENT_NO_CONNECTION,
                };
                xQueueSend(gui_queue, &ev, portMAX_DELAY);
                ec11_set_button_callback(&cb_button_wifi_settings);
                retry_delay_ms = 1 * 1000;
            } else if (failure_count < 6) {
                retry_delay_ms = 1 * 1000;
            } else if (failure_count < 10) {
                retry_delay_ms = 5 * 1000;
            } else if (failure_count < 20) {
                retry_delay_ms = 30 * 1000;
            } else {
                retry_delay_ms = 5 * 60 * 1000;
            }

            if (retry_delay_ms > MAX_BACKOFF_MS) {
                retry_delay_ms = MAX_BACKOFF_MS;
            }

            ESP_LOGI(TAG, "Retrying after %lu ms", (unsigned long)retry_delay_ms);
        }
        esp_http_client_cleanup(client);
    }
}

void userSettings(void *pvParameters) {
    esp_http_client_config_t config = {
        .url = SETTING_URL,
        .event_handler = login_event_handler,
        .timeout_ms = 3000,
        .cert_pem = isrgrootx1_pem_start,
        .method = HTTP_METHOD_POST,
    };
    memset(responseBuffer, 0, sizeof(responseBuffer));
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // 等待通知喚醒
        ESP_LOGI(TAG, "Getting user setting url...");

        char *token = jwt_load_from_nvs();
        static char auth_header[256];
        if (token) {
            snprintf(auth_header, sizeof(auth_header), "Bearer %s", token);
            esp_http_client_set_header(client, "Authorization", auth_header);
            output_len = 0;
            esp_err_t err = esp_http_client_perform(client);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "HTTP response: %s", responseBuffer);
                cJSON *root = cJSON_Parse(responseBuffer);
                if (root) {
                    cJSON *setting_item = cJSON_GetObjectItem(root, "qr_c_array_base64");
                    if (setting_item && cJSON_IsString(setting_item)) {
                        uint8_t buf[256];
                        size_t olen = 0;
                        const char *b64str = setting_item->valuestring; // 你的 base64 字串
                        if (!mbedtls_base64_decode(buf, sizeof(buf), &olen,
                                                   (const unsigned char *)b64str, strlen(b64str))) {

                            // 成功獲取session token
                            ESP_LOGI(TAG, "Decoded base64 string successfully, length: %zu", olen);
                            event_t ev = {
                                .event_id = SCREEN_EVENT_QRCODE,
                            };
                            setting_qrcode_setting((char *)buf);
                            xQueueSend(gui_queue, &ev, portMAX_DELAY);
                            UBaseType_t stack_remain =
                                uxTaskGetStackHighWaterMark(NULL); // NULL 代表查詢目前 task
                            ESP_LOGI("TASK", "Stack high water mark: %u words", stack_remain);
                            ec11_set_button_callback(&cb_button_setting_done);
                        }
                    } else {
                        ESP_LOGE(TAG, "No qrcode in JSON response!");
                    }
                    cJSON_Delete(root);
                    xEventGroupSetBits(net_event_group, NET_TOKEN_AVAILABLE_BIT);
                } else {
                    ESP_LOGE(TAG, "Failed to parse JSON: %s", responseBuffer);
                }
            } else {
                ESP_LOGE(TAG, "failed: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGE(TAG, "No token found, skipping user settings fetch.");
            xEventGroupClearBits(net_event_group, NET_TOKEN_AVAILABLE_BIT);
            xTaskNotifyGive(xServerLoginHandle); // 喚醒 server_login
        }
    }
    esp_http_client_cleanup(client);
}

void netStartup(void *pvParameters) {
    wifi_manager_start();
    wifi_manager_set_callback(WM_EVENT_STA_GOT_IP, &cb_connection_ok);
    wifi_manager_set_callback(WM_ORDER_START_AP, &cb_wifi_required);
    xTaskCreate(server_check_task, "server_check_task", 4096, NULL, 5, &xServerCheckHandle);
    xTaskCreate(server_login, "server_login", 4096, NULL, 5, &xServerLoginHandle);
    xTaskCreate(userSettings, "userSettings", 8192, NULL, 5, &xUserSettingsHandle);
    xTaskCreate(esp_check_auth_result, "esp_check_auth_result", 4096, NULL, 5,
                &xEspCheckAuthResultHandle);
    net_event_group = xEventGroupCreate();
    vTaskDelete(NULL);
}