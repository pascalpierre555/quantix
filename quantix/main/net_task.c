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
SemaphoreHandle_t xWifi;

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
TaskHandle_t xServerCheckCallbackHandle = NULL;
TaskHandle_t xButtonSettingDoneHandle = NULL;
TaskHandle_t xCbContinueNoWifiHandle =
    NULL; // Renamed for clarity: Handle for cb_button_continue_without_wifi task
EventGroupHandle_t net_event_group;

static int output_len = 0; // Stores number of bytes read

// 定義 HTTP 事件處理函式
esp_err_t _http_event_handler(esp_http_client_event_t *evt) { return ESP_OK; }

// 定義登入事件處理函式
esp_err_t get_response_event_handler(esp_http_client_event_t *evt) {

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        // Response data will be read manually in the net_worker_task
        // if event.save_to_buffer is true.
        // If you still want to use the event handler to accumulate data,
        // evt->user_data should point to a structure that includes the buffer,
        // its total size, and the current offset for writing.
        // For simplicity with manual read, we don't accumulate here.
        break;
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
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
            int try_count = 0;
            const int max_retry = 5; // 寫死最大重試次數
            int delay_ms = 1000;     // 寫死初始延遲
            int failure_count = 0;
            int success_count = 0;
            const int max_backoff_ms = MAX_BACKOFF_MS;
            esp_err_t err; // Initialize err for this attempt
            while (1) {
                xSemaphoreTake(xWifi, portMAX_DELAY);
                // 設定 HTTP config
                xEventGroupWaitBits(net_event_group, NET_WIFI_CONNECTED_BIT, false, true,
                                    portMAX_DELAY);
                esp_http_client_config_t config = {
                    .url = event.url,
                    .method = event.method,
                    .timeout_ms = 5000,
                    .event_handler = get_response_event_handler,
                    .cert_pem = isrgrootx1_pem_start,
                    .user_data = event.save_to_buffer ? event.response_buffer : NULL,
                };
                err = ESP_OK;
                esp_http_client_handle_t client = esp_http_client_init(&config);

                if (event.use_jwt) {
                    char *token = jwt_load_from_nvs();
                    if (token) {
                        static char auth_header[256];
                        snprintf(auth_header, sizeof(auth_header), "Bearer %s", token);
                        esp_http_client_set_header(client, "Authorization", auth_header);
                    }
                }
                esp_http_client_set_header(client, "Content-Type", "application/json");
                esp_http_client_set_header(client, "Accept-Encoding", "identity");

                int post_len = 0;
                if (event.method == HTTP_METHOD_POST && event.post_data) {
                    post_len = strlen(event.post_data);
                }

                err = esp_http_client_open(client, post_len);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
                } else {
                    if (event.method == HTTP_METHOD_POST && event.post_data) {
                        int bytes_written =
                            esp_http_client_write(client, event.post_data, post_len);
                        if (bytes_written < 0) {
                            ESP_LOGE(TAG, "esp_http_client_write failed: %s",
                                     esp_err_to_name(bytes_written));
                            err = bytes_written; // Propagate error
                        } else if (bytes_written != post_len) {
                            ESP_LOGW(TAG, "esp_http_client_write sent %d bytes, expected %d",
                                     bytes_written, post_len);
                            err = ESP_FAIL; // Treat as error for simplicity
                        }
                    }

                    if (err == ESP_OK) { // Only proceed if open (and write if POST) was successful
                        int fetch_header_ret = esp_http_client_fetch_headers(client);
                        if (fetch_header_ret < 0) {
                            ESP_LOGE(TAG, "esp_http_client_fetch_headers failed: %s",
                                     esp_err_to_name(fetch_header_ret));
                            err = fetch_header_ret; // Propagate error
                        } else {
                            // Headers fetched, now get status and content length for reading
                            // response
                            if (event.save_to_buffer && event.response_buffer) {
                                int response_content_length =
                                    esp_http_client_get_content_length(client);
                                int total_read_len = 0;
                                event.response_buffer[0] = '\0'; // Initialize to empty string

                                if (response_content_length > 0) {
                                    if (response_content_length < event.response_buffer_size) {
                                        total_read_len = esp_http_client_read_response(
                                            client, event.response_buffer, response_content_length);
                                    } else {
                                        ESP_LOGW(TAG,
                                                 "Response content (%d) larger than buffer (%d). "
                                                 "Truncating.",
                                                 response_content_length,
                                                 event.response_buffer_size);
                                        total_read_len = esp_http_client_read_response(
                                            client, event.response_buffer,
                                            event.response_buffer_size - 1);
                                        // Consume and discard rest of the body
                                        if (total_read_len >= 0) {
                                            char discard_buf[64];
                                            while (esp_http_client_read(client, discard_buf,
                                                                        sizeof(discard_buf)) > 0)
                                                ;
                                        }
                                    }
                                    if (total_read_len < 0) {
                                        ESP_LOGE(TAG, "Failed to read response data: %s",
                                                 esp_err_to_name(total_read_len));
                                        err = total_read_len; // Propagate error from read
                                        total_read_len = 0;   // No valid data read
                                    }
                                } else if (response_content_length == 0) {
                                    total_read_len = 0; // No content
                                } else { // response_content_length == -1 (chunked or connection
                                         // close)
                                    int read_len;
                                    while (total_read_len < event.response_buffer_size - 1) {
                                        read_len = esp_http_client_read(
                                            client, event.response_buffer + total_read_len,
                                            event.response_buffer_size - 1 - total_read_len);
                                        if (read_len < 0) {
                                            ESP_LOGE(TAG, "Read failed (chunked/unknown): %s",
                                                     esp_err_to_name(read_len));
                                            err = read_len; // Propagate error
                                            break;
                                        } else if (read_len == 0) { // End of stream
                                            if (esp_http_client_is_complete_data_received(client)) {
                                                break;
                                            } else {
                                                ESP_LOGW(TAG, "Connection closed prematurely "
                                                              "(chunked/unknown)");
                                                err = ESP_FAIL; // Optional: treat as error if
                                                // data is incomplete
                                                break;
                                            }
                                        }
                                        total_read_len += read_len;
                                    }
                                }
                                if (event.response_buffer) {
                                    event.response_buffer[total_read_len] = '\0'; // Null terminate
                                }
                            }
                        }
                    }
                }

                esp_http_client_close(client); // Close connection
                xSemaphoreGive(xWifi);         // Release semaphore AFTER all

                // 自動解析 JSON
                if (err == ESP_OK && event.json_parse && event.response_buffer) {
                    cJSON *parsed_json = cJSON_Parse(event.response_buffer);
                    if (!parsed_json) {
                        ESP_LOGW(TAG, "Failed to parse JSON from response: %s",
                                 event.response_buffer);
                    }
                    event.json_root = parsed_json; // Store parsed result (or NULL if failed)
                } else if (event.json_root !=
                           NULL) { // If parsing was requested but HTTP failed or no buffer

                    event.json_root = NULL;
                }

                bool should_retry = false;
                if (err == ESP_OK) {
                    success_count++;
                    failure_count = 0;
                } else {
                    failure_count++;
                    should_retry = (try_count < max_retry);
                }

                esp_http_client_cleanup(client);

                if (!should_retry)
                    break;

                // 退避邏輯
                if (failure_count == 1) {
                    delay_ms = 1000;
                } else if (failure_count < 6) {
                    delay_ms = 1000;
                } else if (failure_count < 10) {
                    delay_ms = 5000;
                } else if (failure_count < 20) {
                    delay_ms = 30000;
                } else {
                    delay_ms = 5 * 60 * 1000;
                }
                if (delay_ms > max_backoff_ms)
                    delay_ms = max_backoff_ms;

                ESP_LOGW("NET_TASK", "Request failed, retry %d/%d after %d ms", try_count + 1,
                         max_retry, delay_ms);
                vTaskDelay(delay_ms / portTICK_PERIOD_MS);
                try_count++;
            }
            if (event.on_finish) {
                event.on_finish(&event, err);
            }
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

void cb_button_wifi_settings(void *pvParameters) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ec11_clean_button_callback();
        xSemaphoreTake(xWifi, portMAX_DELAY);
        xEventGroupClearBits(net_event_group, NET_WIFI_CONNECTED_BIT);
        wifi_manager_send_message(WM_ORDER_DISCONNECT_STA, NULL);
        xSemaphoreGive(xWifi);
    }
}

void cb_button_continue_without_wifi(void *pvParameters) {
    for (;;) {
        // Wait for a notification, presumably from an ISR (e.g., EC11 button press)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // It's good practice to clean the button callback if this action is one-shot
        // or if another callback should take over.
        ec11_clean_button_callback();

        if (!isr_woken) {
            xTaskNotify(xCalendarDisplayHandle, 0, eSetValueWithOverwrite);
        }
    }
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
                ev.msg[26] = '\0'; // 確保string結尾
                strncat(ev.msg, status, MAX_MSG_LEN - strlen(ev.msg) - 1);
                xQueueSend(gui_queue, &ev, portMAX_DELAY);
                xSemaphoreTake(xScreen, portMAX_DELAY);
                ec11_set_button_callback(xButtonSettingDoneHandle);
                xSemaphoreGive(xScreen);
                xQueueSend(gui_queue, &evqr, portMAX_DELAY);
            }
        } else {
            http_response_save_to_nvs(event->json_root, "calendar", "access_token");
            http_response_save_to_nvs(event->json_root, "calendar", "refresh_token");
            xEventGroupSetBits(net_event_group, NET_GOOGLE_TOKEN_AVAILABLE_BIT);
            xTaskNotifyGive(xCalendarPrefetchHandle);
            if (!isr_woken) {
                xTaskNotify(xCalendarDisplayHandle, 0, eSetValueWithOverwrite);
            }
        }
        cJSON_Delete(event->json_root); // 用完要釋放
    } else {
        ESP_LOGE(TAG, "Failed to parse JSON or HTTP error: %s",
                 event->response_buffer ? event->response_buffer : "");
    }
}

void cb_button_setting_done(void *pvParameters) {
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
        .json_parse = 1,
    };
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ec11_clean_button_callback();
        xQueueSend(net_queue, &event, portMAX_DELAY);
    }
}

static void server_check_callback(net_event_t *event, esp_err_t err) {
    if (err == ESP_OK && event->json_root) {
        cJSON *token_item = cJSON_GetObjectItem(event->json_root, "token");
        if (token_item && cJSON_IsString(token_item)) {
            const char *jwt = token_item->valuestring;
            jwt_save_to_nvs(jwt);
            ESP_LOGI(TAG, "Login success, token saved.");
            xEventGroupSetBits(net_event_group, NET_TOKEN_AVAILABLE_BIT);
            xEventGroupSetBits(net_event_group, NET_SERVER_CONNECTED_BIT);
            if (!isr_woken) {
                event_t ev = {
                    .event_id = SCREEN_EVENT_CENTER,
                    .msg = "Server connected successfully:)",
                };
                xQueueSend(gui_queue, &ev, portMAX_DELAY);
            }
            xTaskNotifyGive(xCalendarPrefetchHandle);
            if (!isr_woken) {
                xTaskNotify(xCalendarDisplayHandle, 0, eSetValueWithOverwrite);
            }
        } else {
            ESP_LOGE(TAG, "No 'token' in JSON response!");
        }
        cJSON_Delete(event->json_root);
    } else {
        ESP_LOGE(TAG, "Login failed or JSON parse error: %s, err: %d",
                 event->response_buffer ? event->response_buffer : "", err);
        xEventGroupClearBits(net_event_group, NET_SERVER_CONNECTED_BIT);
        event_t ev = {
            .event_id = SCREEN_EVENT_NO_CONNECTION,
            .msg = "Server connection failed, please check your network settings.",
        };
        xQueueSend(gui_queue, &ev, portMAX_DELAY);
        ec11_set_button_callback(xServerCheckCallbackHandle);
    }
}

// userSettings 的 callback
static void user_settings_callback(net_event_t *event, esp_err_t err) {
    if (err == ESP_OK && event->json_root) {
        ESP_LOGI(TAG, "HTTP response: %s", event->response_buffer);
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
                ec11_set_button_callback(xButtonSettingDoneHandle);
            }
        } else {
            ESP_LOGE(TAG, "No qrcode in JSON response!");
        }
        cJSON_Delete(event->json_root);
    } else {
        ESP_LOGE(TAG, "Failed to parse JSON or HTTP error: %s",
                 event->response_buffer ? event->response_buffer : "");
        xEventGroupClearBits(net_event_group, NET_TOKEN_AVAILABLE_BIT);
    }
}

void cb_connection_ok(void *pvParameter) {
    ip_event_got_ip_t *param = (ip_event_got_ip_t *)pvParameter;

    /* transform IP to human readable string */
    char str_ip[16];
    esp_ip4addr_ntoa(&param->ip_info.ip, str_ip, IP4ADDR_STRLEN_MAX);

    ESP_LOGI(TAG, "I have a connection and my IP is %s!", str_ip);
    xEventGroupSetBits(net_event_group, NET_WIFI_CONNECTED_BIT);
    if (!isr_woken) {
        event_t ev = {
            .event_id = SCREEN_EVENT_CENTER,
            .msg = "WiFi connected:)",
        };
        xQueueSend(gui_queue, &ev, portMAX_DELAY);
    }
    xSemaphoreGive(xWifi);
    server_check(); // 檢查伺服器連線
}

void cb_wifi_required(void *pvParameter) {
    ESP_LOGI(TAG, "WiFi required, switching to AP mode...");
    event_t ev = {
        .event_id = SCREEN_EVENT_WIFI_REQUIRED,
        .msg = "WiFi required, switching to AP mode",
    };
    xQueueSend(gui_queue, &ev, portMAX_DELAY);
    ec11_set_button_callback(xCbContinueNoWifiHandle);
}

void server_check(void) {
    net_event_t event = {
        .url = LOGIN_URL,
        .method = HTTP_METHOD_POST,
        .post_data = "{\"username\":\"esp32\", \"password\":\"supersecret\"}",
        .use_jwt = false,
        .save_to_buffer = true,
        .response_buffer = responseBuffer,
        .response_buffer_size = sizeof(responseBuffer),
        .on_finish = server_check_callback,
        .user_data = NULL,
        .json_parse = 1,
    };
    xQueueSend(net_queue, &event, portMAX_DELAY);
}

void userSettings(void) {
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
            .json_parse = 1, // 只要不是NULL就會自動parse
        };
        xQueueSend(net_queue, &event, portMAX_DELAY);
    } else {
        ESP_LOGE(TAG, "No token found, skipping user settings fetch.");
        xEventGroupClearBits(net_event_group, NET_TOKEN_AVAILABLE_BIT);
        server_check(); // 如果沒有 token，就直接檢查伺服器連線
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
    xTaskCreate(net_worker_task, "net_worker_task", 8192, NULL, 4, NULL);
    xTaskCreate(cb_button_wifi_settings, "cb_button_wifi_settings", 2048, NULL, 4,
                &xServerCheckCallbackHandle);
    xTaskCreate(cb_button_setting_done, "cb_button_setting_done", 2048, NULL, 4,
                &xButtonSettingDoneHandle);
    xTaskCreate(cb_button_continue_without_wifi, "cb_button_continue_without_wifi", 2048, NULL, 4,
                &xCbContinueNoWifiHandle);
    vTaskDelete(NULL);
}