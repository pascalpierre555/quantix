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

#define NET_QUEUE_SIZE 8
QueueHandle_t net_queue;

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
TaskHandle_t xEspCheckAuthResultHandle = NULL;

EventGroupHandle_t net_event_group;

static int output_len = 0; // Stores number of bytes read

// 定義 HTTP 事件處理函式
esp_err_t _http_event_handler(esp_http_client_event_t *evt) { return ESP_OK; }

// 定義登入事件處理函式
esp_err_t get_response_event_handler(esp_http_client_event_t *evt) {

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

void net_worker_task(void *pvParameters) {
    net_event_t event;
    for (;;) {
        if (xQueueReceive(net_queue, &event, portMAX_DELAY) == pdTRUE) {
            // 設定 HTTP config
            esp_http_client_config_t config = {
                .url = event.url,
                .method = event.method,
                .timeout_ms = 5000,
                .event_handler = get_response_event_handler, // 你原本的 handler
                .cert_pem = isrgrootx1_pem_start,            // 使用 ISRG Root X1 憑證
                .user_data = event.save_to_buffer ? event.response_buffer : NULL,
            };
            esp_http_client_handle_t client = esp_http_client_init(&config);

            if (event.use_jwt) {
                char *token = jwt_load_from_nvs();
                if (token) {
                    static char auth_header[256];
                    snprintf(auth_header, sizeof(auth_header), "Bearer %s", token);
                    esp_http_client_set_header(client, "Authorization", auth_header);
                }
            }
            if (event.method == HTTP_METHOD_POST && event.post_data) {
                esp_http_client_set_post_field(client, event.post_data, strlen(event.post_data));
            }
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_header(client, "Accept-Encoding", "identity");

            esp_err_t err = esp_http_client_perform(client);

            // 若要存回應
            if (event.save_to_buffer && event.response_buffer) {
                int len = esp_http_client_get_content_length(client);
                if (len > 0 && len < event.response_buffer_size) {
                    esp_http_client_read(client, event.response_buffer, len);
                    event.response_buffer[len] = '\0';
                }
            }

            // 新增：自動解析 JSON
            if (event.json_root != NULL && event.response_buffer) {
                event.json_root = cJSON_Parse(event.response_buffer);
            } else {
                event.json_root = NULL;
            }

            if (event.on_finish) {
                event.on_finish(&event, err);
            }

            // 若有 json_root，記得 callback 用完要 cJSON_Delete
            esp_http_client_cleanup(client);
        }
    }
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

// server_check_task 的 callback
static void server_check_callback(net_event_t *event, esp_err_t err) {
    static uint8_t success_count = 0;
    static uint8_t failure_count = 0;
    static TickType_t retry_delay_ms = 1000;

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Server connected successfully");
        success_count++;
        failure_count = 0;
        retry_delay_ms = 1000;
        xEventGroupSetBits(net_event_group, NET_SERVER_CONNECTED_BIT);
        event_t ev = {
            .event_id = SCREEN_EVENT_CENTER,
            .msg = "Server connected successfully:)",
        };
        xQueueSend(gui_queue, &ev, portMAX_DELAY);
        xTaskNotifyGive(xcalendarStartupHandle);
        vTaskSuspend(NULL);
    } else {
        xEventGroupClearBits(net_event_group, NET_SERVER_CONNECTED_BIT);
        success_count = 0;
        failure_count++;
        ESP_LOGW(TAG, "Server connect failed, count: %d", failure_count);

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
        vTaskDelay(retry_delay_ms / portTICK_PERIOD_MS);
    }
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
    esp_check_auth_result();
}

// 新增 callback 處理 HTTP 結果
static void check_auth_result_callback(net_event_t *event, esp_err_t err) {
    event_t ev = {
        .event_id = SCREEN_EVENT_CENTER,
        .msg = "Please try again, status: ",
    };
    event_t evqr = {
        .event_id = SCREEN_EVENT_QRCODE,
    };

    if (err == ESP_OK && event->json_root) {
        ESP_LOGI(TAG, "HTTP response: %s", event->response_buffer);
        if (http_response_save_to_nvs(event->json_root, "calendar", "email")) {
            cJSON *item = cJSON_GetObjectItem(event->json_root, "status");
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
            http_response_save_to_nvs(event->json_root, "calendar", "access_token");
            http_response_save_to_nvs(event->json_root, "calendar", "refresh_token");
            xTaskNotifyGive(xcalendarStartupHandle);
        }
        cJSON_Delete(event->json_root); // 用完要釋放
    } else {
        ESP_LOGE(TAG, "Failed to parse JSON or HTTP error: %s",
                 event->response_buffer ? event->response_buffer : "");
    }
}

// 登入結果 callback
static void server_login_callback(net_event_t *event, esp_err_t err) {
    if (err == ESP_OK && event->json_root) {
        cJSON *token_item = cJSON_GetObjectItem(event->json_root, "token");
        if (token_item && cJSON_IsString(token_item)) {
            const char *jwt = token_item->valuestring;
            jwt_save_to_nvs(jwt);
            ESP_LOGI(TAG, "Login success, token saved.");
            xEventGroupSetBits(net_event_group, NET_TOKEN_AVAILABLE_BIT);
        } else {
            ESP_LOGE(TAG, "No 'token' in JSON response!");
        }
        cJSON_Delete(event->json_root);
    } else {
        ESP_LOGE(TAG, "Login failed or JSON parse error: %s",
                 event->response_buffer ? event->response_buffer : "");
        xEventGroupClearBits(net_event_group, NET_SERVER_CONNECTED_BIT);
        vTaskResume(xServerCheckHandle);     // 喚醒 server_check_task
        xTaskNotifyGive(xServerCheckHandle); // 喚醒 server_check_task
    }
}

// userSettings 的 callback
static void user_settings_callback(net_event_t *event, esp_err_t err) {
    if (err == ESP_OK && event->json_root) {
        cJSON *setting_item = cJSON_GetObjectItem(event->json_root, "qr_c_array_base64");
        if (setting_item && cJSON_IsString(setting_item)) {
            uint8_t buf[256];
            size_t olen = 0;
            const char *b64str = setting_item->valuestring;
            if (!mbedtls_base64_decode(buf, sizeof(buf), &olen, (const unsigned char *)b64str,
                                       strlen(b64str))) {
                ESP_LOGI(TAG, "Decoded base64 string successfully, length: %zu", olen);
                event_t ev = {
                    .event_id = SCREEN_EVENT_QRCODE,
                };
                setting_qrcode_setting((char *)buf);
                xQueueSend(gui_queue, &ev, portMAX_DELAY);
                UBaseType_t stack_remain = uxTaskGetStackHighWaterMark(NULL);
                ESP_LOGI("TASK", "Stack high water mark: %u words", stack_remain);
                ec11_set_button_callback(&cb_button_setting_done);
            }
        } else {
            ESP_LOGE(TAG, "No qrcode in JSON response!");
        }
        cJSON_Delete(event->json_root);
        xEventGroupSetBits(net_event_group, NET_TOKEN_AVAILABLE_BIT);
    } else {
        ESP_LOGE(TAG, "Failed to parse JSON or HTTP error: %s",
                 event->response_buffer ? event->response_buffer : "");
        xEventGroupClearBits(net_event_group, NET_TOKEN_AVAILABLE_BIT);
        xTaskNotifyGive(xServerLoginHandle); // 喚醒 server_login
    }
}

void esp_check_auth_result(void) {
    net_event_t event = {
        .url = CHECK_AUTH_RESULT_URL,
        .method = HTTP_METHOD_GET,
        .post_data = NULL,
        .use_jwt = true,
        .save_to_buffer = true,
        .response_buffer = responseBuffer,
        .response_buffer_size = sizeof(responseBuffer),
        .on_finish = check_auth_result_callback,
        .user_data = NULL,
        .json_root = (void *)1,
    };
    xQueueSend(net_queue, &event, portMAX_DELAY);
}

// 改寫後的 server_login
void server_login(void *pvParameter) {

    // char *buf = malloc((buf_size + 1) * sizeof(char));
    const char *post_data = "{\"username\":\"esp32\", \"password\":\"supersecret\"}";
    TickType_t token_expire_time = 60 * 60 * 1000; // 1小時

    for (;;) {
        ulTaskNotifyTake(pdTRUE, token_expire_time / portTICK_PERIOD_MS); // 等待通知喚醒
        ESP_LOGI(TAG, "No token found. Logging in...");
        xEventGroupClearBits(net_event_group, NET_TOKEN_AVAILABLE_BIT);

        net_event_t event = {
            .url = LOGIN_URL,
            .method = HTTP_METHOD_POST,
            .post_data = post_data,
            .use_jwt = false,
            .save_to_buffer = true,
            .response_buffer = responseBuffer,
            .response_buffer_size = sizeof(responseBuffer),
            .on_finish = server_login_callback,
            .user_data = NULL,
            .json_root = (void *)1, // 只要不是NULL就會自動parse
        };
        xQueueSend(net_queue, &event, portMAX_DELAY);

        // 等待 NET_TOKEN_AVAILABLE_BIT 設定或逾時
        EventBits_t bits = xEventGroupWaitBits(net_event_group, NET_TOKEN_AVAILABLE_BIT, pdFALSE,
                                               pdFALSE, token_expire_time);
        if (bits & NET_TOKEN_AVAILABLE_BIT) {
            token_expire_time = 60 * 60 * 1000; // 成功則重設為1小時
        } else {
            token_expire_time = 500; // 失敗則快速重試
        }
    }
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
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xEventGroupWaitBits(net_event_group, NET_WIFI_CONNECTED_BIT, pdFALSE, pdFALSE,
                            portMAX_DELAY);
        net_event_t event = {
            .url = TEST_URL,
            .method = HTTP_METHOD_GET,
            .post_data = NULL,
            .use_jwt = false,
            .save_to_buffer = false,
            .response_buffer = NULL,
            .response_buffer_size = 0,
            .on_finish = server_check_callback,
            .user_data = NULL,
            .json_root = NULL, // 不需要 parse JSON
        };
        xQueueSend(net_queue, &event, portMAX_DELAY);
    }
}

void userSettings(void) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // 等待通知喚醒
    ESP_LOGI(TAG, "Getting user setting url...");

    char *token = jwt_load_from_nvs();
    if (token) {
        net_event_t event = {
            .url = SETTING_URL,
            .method = HTTP_METHOD_POST,
            .post_data = NULL,
            .use_jwt = true,
            .save_to_buffer = true,
            .response_buffer = responseBuffer,
            .response_buffer_size = sizeof(responseBuffer),
            .on_finish = user_settings_callback,
            .user_data = NULL,
            .json_root = (void *)1, // 只要不是NULL就會自動parse
        };
        xQueueSend(net_queue, &event, portMAX_DELAY);
    } else {
        ESP_LOGE(TAG, "No token found, skipping user settings fetch.");
        xEventGroupClearBits(net_event_group, NET_TOKEN_AVAILABLE_BIT);
        xTaskNotifyGive(xServerLoginHandle); // 喚醒 server_login
    }
}

void download_calendar_data_task(void *pvParameters) {
    esp_http_client_config_t config = {
        .url = SETTING_URL,
        .event_handler = get_response_event_handler,
        .timeout_ms = 3000,
        .cert_pem = isrgrootx1_pem_start,
        .method = HTTP_METHOD_POST,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
}

void netStartup(void *pvParameters) {
    wifi_manager_start();
    wifi_manager_set_callback(WM_EVENT_STA_GOT_IP, &cb_connection_ok);
    wifi_manager_set_callback(WM_ORDER_START_AP, &cb_wifi_required);
    net_queue = xQueueCreate(NET_QUEUE_SIZE, sizeof(net_event_t));
    xTaskCreate(net_worker_task, "net_worker_task", 8192, NULL, 5, NULL);

    xTaskCreate(server_check_task, "server_check_task", 4096, NULL, 5, &xServerCheckHandle);
    xTaskCreate(server_login, "server_login", 4096, NULL, 5, &xServerLoginHandle);
    net_event_group = xEventGroupCreate();
    vTaskDelete(NULL);
}