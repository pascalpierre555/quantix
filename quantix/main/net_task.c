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

#define NET_QUEUE_SIZE 13
QueueHandle_t net_queue;
SemaphoreHandle_t xWifi;

// Root CA certificate for the server.
extern const char isrgrootx1_pem_start[] asm("_binary_isrgrootx1_pem_start");
extern const char isrgrootx1_pem_end[] asm("_binary_isrgrootx1_pem_end");

static char responseBuffer[512];

// URLs for the backend server.
#define LOGIN_URL "https://peng-pc.tail941dce.ts.net/login"
#define TEST_URL "https://peng-pc.tail941dce.ts.net/ping"
#define SETTING_URL "https://peng-pc.tail941dce.ts.net/settings"
#define CHECK_AUTH_RESULT_URL "https://peng-pc.tail941dce.ts.net/check_auth_result?username=esp32"
// Maximum backoff time is 15 minutes.
#define MAX_BACKOFF_MS 15 * 60 * 1000

// Task handles for network-related tasks.
TaskHandle_t xServerCheckHandle = NULL;
TaskHandle_t xServerLoginHandle = NULL;
TaskHandle_t xEspCheckAuthResultHandle = NULL;
TaskHandle_t xServerCheckCallbackHandle = NULL;
TaskHandle_t xButtonSettingDoneHandle = NULL;
// Handle for cb_button_continue_without_wifi task
TaskHandle_t xCbContinueNoWifiHandle = NULL;
EventGroupHandle_t net_event_group;

/**
 * @brief HTTP event handler for logging purposes.
 *
 * This handler logs various HTTP client events to the console for debugging.
 * It does not accumulate response data, as that is handled manually in the worker task.
 *
 * @param evt Pointer to the HTTP client event data.
 * @return ESP_OK always.
 */
esp_err_t get_response_event_handler(esp_http_client_event_t *evt) {

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
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

/**
 * @brief Callback to handle the result of the initial server login/check.
 *
 * This function is called after the device attempts to log in to the server.
 * If successful, it saves the received JWT, sets event group bits to signal
 * that the server is connected and a token is available, and notifies other tasks
 * to proceed with their network-dependent operations. If it fails, it updates the
 * UI to show a connection error and sets up a button callback to retry.
 *
 * @param event The network event containing the response data.
 * @param err The result of the HTTP request.
 */
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

/**
 * @brief Callback to handle the response from the user settings request.
 *
 * This function is called after the device requests user settings. It expects
 * a JSON response containing a base64-encoded QR code. It decodes the string
 * and passes the QR code data to the UI task to be displayed.
 *
 * @param event The network event containing the response data.
 * @param err The result of the HTTP request.
 */
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

/**
 * @brief Queues a request to log in to the server and obtain a JWT.
 *
 * This function constructs and sends a `net_event_t` to the `net_queue` for the
 * worker task to process.
 */

void server_check(void) {
    net_event_t event = {
        .url = LOGIN_URL,
        .method = HTTP_METHOD_POST,
        .post_data = "{\"username\":\"esp32\", \"password\":\"supersecret\"}",
        .use_jwt = false,
        .response_buffer = responseBuffer,
        .response_buffer_size = sizeof(responseBuffer),
        .on_finish = server_check_callback,
        .user_data = NULL,
    };
    xQueueSend(net_queue, &event, portMAX_DELAY);
    return;
}

/**
 * @brief Queues a request to fetch user-specific settings from the server.
 *
 * This function is typically called when the user needs to configure their account
 * with the device. It retrieves the JWT from NVS and sends an authenticated request.
 * If no token is found, it initiates a `server_check` to log in first.
 */

void userSettings(void) {
    ESP_LOGI(TAG, "Getting user setting url...");
    net_event_t event = {
        .url = SETTING_URL,
        .method = HTTP_METHOD_POST,
        .post_data = NULL,
        .use_jwt = true,
        .response_buffer = responseBuffer,
        .response_buffer_size = sizeof(responseBuffer),
        .on_finish = user_settings_callback,
        .user_data = NULL,
    };
    xQueueSendToFront(net_queue, &event, portMAX_DELAY);
    return;
}

/**
 * @brief A worker task that processes network requests from a queue.
 *
 * This task waits for `net_event_t` items on `net_queue`. For each event, it
 * performs an HTTP request with retry logic and exponential backoff. It handles
 * setting JWT headers, posting data, receiving responses, and optionally
 * parsing the response as JSON. After the request is complete (or has failed
 * after all retries), it calls the `on_finish` callback specified in the event.
 *
 * @param pvParameters Unused.
 */
void net_worker_task(void *pvParameters) {
    net_event_t event;
    static char auth_header[256]; // Buffer for jwt token header
    const uint32_t max_backoff_ms = MAX_BACKOFF_MS;
    const uint8_t max_retry = 5; // Hardcoded maximum number of retries.
    int http_status_code = 0;
    esp_err_t err; // Initialize err for this attempt
    uint8_t try_count;
    uint32_t delay_ms; // Hardcoded initial delay.
    uint8_t failure_count;
    uint8_t success_count;
    for (;;) {
        if (xQueueReceive(net_queue, &event, portMAX_DELAY) == pdTRUE) {
            try_count = 0;
            delay_ms = 1000; // Hardcoded initial delay.
            failure_count = 0;
            success_count = 0;
            while (1) {
                xSemaphoreTake(xWifi, portMAX_DELAY);
                // Configure the HTTP client.
                xEventGroupWaitBits(net_event_group, NET_WIFI_CONNECTED_BIT, false, true,
                                    portMAX_DELAY);
                esp_http_client_config_t config = {
                    .url = event.url,
                    .method = event.method,
                    .timeout_ms = 5000,
                    .event_handler = get_response_event_handler,
                    .cert_pem = isrgrootx1_pem_start,
                    .user_data = event.response_buffer,
                };
                err = ESP_OK;
                esp_http_client_handle_t client = esp_http_client_init(&config);

                if (event.use_jwt) {
                    char *token = jwt_load_from_nvs();
                    if (token) {
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
                            http_status_code =
                                esp_http_client_get_status_code(client); // Get HTTP status code
                            // Headers fetched, now get status and content length for reading
                            // response
                            if (event.response_buffer) {
                                int response_content_length =
                                    esp_http_client_get_content_length(client);
                                int total_read_len = 0;
                                event.response_buffer[0] = '\0';

                                if (response_content_length >
                                    0) { // If there's response content, then read it
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
                                    if (total_read_len < 0) { // Read failed, connection closed
                                        ESP_LOGE(TAG, "Failed to read response data: %s",
                                                 esp_err_to_name(total_read_len));
                                        err = total_read_len;
                                        total_read_len = 0;
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

                // Automatically parse JSON if requested.
                if (err == ESP_OK && event.response_buffer) {
                    cJSON *parsed_json = cJSON_Parse(event.response_buffer);
                    if (!parsed_json) {
                        ESP_LOGW(TAG, "Failed to parse JSON from response: %s",
                                 event.response_buffer);
                    }
                    event.json_root = parsed_json; // Store parsed result (or NULL if failed)
                    if (http_status_code >= 400) {
                        const char *error_message = NULL;
                        if (parsed_json) {
                            cJSON *item = cJSON_GetObjectItem(parsed_json, "error");
                            error_message = cJSON_GetStringValue(item);
                        }

                        err = ESP_FAIL; // Treat HTTP errors as failures for retry logic.

                        if (error_message) {
                            ESP_LOGE(TAG, "HTTP Error %d for URL %s: %s", http_status_code,
                                     event.url, error_message);
                            if (strncmp(error_message, "JWT", 3) == 0) {
                                server_check();
                            } else if (strncmp(error_message, "Google", 6) == 0) {
                                userSettings();
                            }
                            try_count = max_retry; // Force exit from the retry loop.
                        }
                    }
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

                // Backoff logic for retries.
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

                ESP_LOGW("NET_TASK", "Request failed, retry %u/%u after %lu ms", try_count + 1,
                         max_retry, (unsigned long)delay_ms);
                vTaskDelay(delay_ms / portTICK_PERIOD_MS);
                try_count++;
            }
            if (event.on_finish) {
                event.on_finish(&event, err);
            }
        }
    }
}

/**
 * @brief Saves a string value from a cJSON object to NVS.
 *
 * @param root The root cJSON object.
 * @param nvs_name The name of the NVS namespace to open.
 * @param key The key of the string item to find in the JSON object and use for NVS storage.
 * @return 0 on success, 1 on failure.
 */
bool http_response_save_to_nvs(cJSON *root, char *nvs_name, char *key) {
    if (root && key) {
        cJSON *item = cJSON_GetObjectItem(root, key);
        if (item && cJSON_IsString(item)) {
            // Save to NVS.
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

/**
 * @brief Task acting as a callback for a button press to enter Wi-Fi settings mode.
 *
 * This task waits for a notification, then disconnects the device from the current
 * Wi-Fi station, which will trigger the wifi_manager to enter AP mode for configuration.
 *
 * @param pvParameters Unused.
 */
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

/**
 * @brief Task acting as a callback for a button press to continue without a Wi-Fi connection.
 *
 * This task waits for a notification (typically from a button press ISR) and then
 * notifies the calendar display task to proceed, for example, by showing cached data.
 *
 * @param pvParameters Unused.
 */
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

/**
 * @brief Callback to handle the result of the authentication check with the server.
 *
 * This function is called after the device has requested to check the result of a
 * user authentication process (e.g., after scanning a QR code). It parses the
 * server's response. If successful, it saves the new tokens. If pending or failed,
 * it updates the UI accordingly.
 *
 * @param event The network event containing the response data.
 * @param err The result of the HTTP request.
 */
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
                ev.msg[26] = '\0'; // Ensure string is null-terminated
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
        cJSON_Delete(event->json_root); // Free the parsed JSON
    } else {
        ESP_LOGE(TAG, "Failed to parse JSON or HTTP error: %s",
                 event->response_buffer ? event->response_buffer : "");
    }
}

/**
 * @brief Task acting as a callback for a button press indicating the user has finished the
 * settings process.
 *
 * This task waits for a notification, then queues a network request to check the
 * authentication result with the server.
 * @param pvParameters Unused.
 */
void cb_button_setting_done(void *pvParameters) {
    net_event_t event = {
        .url = CHECK_AUTH_RESULT_URL,
        .method = HTTP_METHOD_GET,
        .post_data = NULL,
        .use_jwt = true,
        .response_buffer = responseBuffer,
        .response_buffer_size = sizeof(responseBuffer),
        .on_finish = check_auth_result_callback,
        .user_data = NULL,
    };
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ec11_clean_button_callback();
        xQueueSend(net_queue, &event, portMAX_DELAY);
    }
}

/**
 * @brief Callback executed when the device successfully connects to Wi-Fi and gets an IP
 * address.
 *
 * It sets the `NET_WIFI_CONNECTED_BIT` in the event group, updates the UI,
 * and triggers an initial check/login with the server.
 *
 * @param pvParameter A pointer to the `ip_event_got_ip_t` event data.
 */
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
    server_check(); // Check server connection
}

/**
 * @brief Callback executed when Wi-Fi credentials are required.
 *
 * This is triggered by the wifi_manager when it cannot connect to a known AP.
 * It queues a message to the UI task to display the Wi-Fi setup screen (with QR code)
 * and sets up a button callback to allow the user to proceed without Wi-Fi.
 *
 * @param pvParameter Unused.
 */
void cb_wifi_required(void *pvParameter) {
    ESP_LOGI(TAG, "WiFi required, switching to AP mode...");
    event_t ev = {
        .event_id = SCREEN_EVENT_WIFI_REQUIRED,
        .msg = "WiFi required, switching to AP mode",
    };
    xQueueSend(gui_queue, &ev, portMAX_DELAY);
    ec11_set_button_callback(xCbContinueNoWifiHandle);
}

/**
 * @brief Initializes the network components and starts all related tasks.
 *
 * This function should be called once at startup. It starts the wifi_manager,
 * sets up callbacks for Wi-Fi events, creates the network request queue, and
 * creates all the tasks responsible for handling network operations and related
 * button callbacks.
 *
 * @param pvParameters Unused.
 */
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