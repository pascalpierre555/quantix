// #include <arpa/inet.h>
// #include <esp_netif.h>
// #include <esp_wifi.h>
// #include <nvs_flash.h>
// #include <stdint.h>
// #include <stdio.h>
// #include <string.h>

// #include "EPD_2in9.h"
// #include "EPD_config.h"
// #include "GUI_Paint.h"
// #include "ImageData.h"
// #include "esp_log.h"
// #include "esp_system.h"
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "ping/ping_sock.h"
// #include "wifi_manager.h"

// /* @brief tag used for ESP serial console messages */
// static const char TAG[] = "main";
// SemaphoreHandle_t xScreen;

// void screenStartup(void *pvParameters) {
//     if (DEV_Module_Init() != 0) {
//     }

//     printf("e-Paper Init and Clear...\r\n");
//     EPD_2IN9_V2_Init();
//     for (uint8_t i = 0; i < 10; i++) {
//         xScreen = xSemaphoreCreateBinary();
//         if (xScreen == NULL) {
//             printf("Failed to create semaphore for screen, retry...\r\n");
//         } else {
//             break;
//         }
//     }

//     EPD_2IN9_V2_Clear();

//     // Create a new image cache
//     UBYTE *BlackImage;
//     UWORD Imagesize =
//         ((EPD_2IN9_V2_WIDTH % 8 == 0) ? (EPD_2IN9_V2_WIDTH / 8)
//                                       : (EPD_2IN9_V2_WIDTH / 8 + 1)) *
//         EPD_2IN9_V2_HEIGHT;
//     if ((BlackImage = (UBYTE *)malloc(Imagesize)) == NULL) {
//         printf("Failed to apply for black memory...\r\n");
//     }
//     Paint_NewImage(BlackImage, EPD_2IN9_V2_WIDTH, EPD_2IN9_V2_HEIGHT, 90,
//                    WHITE);
//     Paint_SelectImage(BlackImage);
//     DEV_Delay_ms(1000);
//     Paint_Clear(WHITE);

//     // Draw qrcode and instruction text
//     char instructionText[] = " Scan QR code  to setup Wi-Fi";
//     Paint_DrawBitMap_Paste(gImage_wifiqrcode, 14, 14, 99, 99, 1);
//     Paint_DrawString_EN(130, 30, instructionText, &Font16, WHITE, BLACK);
//     EPD_2IN9_V2_Display(BlackImage);
//     DEV_Delay_ms(3000);
//     printf("Goto Sleep...\r\n");
//     EPD_2IN9_V2_Sleep();
//     DEV_Delay_ms(2000); // important, at least 2s
//     // close 5V
//     printf("close 5V, Module enters 0 power consumption ...\r\n");
//     vTaskDelete(NULL);
// }

// // get ping result when ping end, if ping success, do nothing, if ping
// // failed, switch to AP mode
// void CallbackOnPingEnd(esp_ping_handle_t hdl, void *args) {
//     uint32_t transmitted = 0, received = 0;
//     esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted,
//                          sizeof(transmitted));
//     esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received,
//     sizeof(received)); esp_ping_delete_session(hdl);

//     if (received == 0) {
//         printf("Ping failed. Switching to AP mode.\n");

//         // 離線 & 啟動 AP
//         wifi_manager_disconnect_async(); // 離線（會觸發 DISCONNECTED 邏輯）
//         wifi_manager_send_message(WM_ORDER_START_AP, NULL); // 啟動 AP
//     } else {
//         printf("Ping success.\n");
//     }
// }

// /**
//  * @brief this is an exemple of a callback that you can setup in your own app
//  to
//  * get notified of wifi manager event.
//  */
// void cb_connection_ok(void *pvParameter) {
//     ip_event_got_ip_t *param = (ip_event_got_ip_t *)pvParameter;

//     /* transform IP to human readable string */
//     char str_ip[16];
//     esp_ip4addr_ntoa(&param->ip_info.ip, str_ip, IP4ADDR_STRLEN_MAX);

//     ESP_LOGI(TAG, "I have a connection and my IP is %s!", str_ip);

//     // 建立 ping session
//     esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
//     ping_config.target_addr.u_addr.ip4.addr = inet_addr("8.8.8.8");
//     ping_config.target_addr.type = ESP_IPADDR_TYPE_V4;
//     ping_config.count = 4;
//     esp_ping_callbacks_t cbs;
//     cbs.on_ping_end = CallbackOnPingEnd;
//     esp_ping_handle_t ping;

//     esp_ping_new_session(&ping_config, &cbs, &ping);
//     esp_ping_start(ping);
// }

// void app_main() {
//     xTaskCreate(screenStartup, "screenStartup", 4096, NULL, 5, NULL);

//     // 啟動 Wi-Fi Manager
//     wifi_manager_start();

//     // 設定回呼
//     wifi_manager_set_callback(WM_EVENT_STA_GOT_IP, &cb_connection_ok);
// }

#include "JWT_storage.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <stdio.h>

#define LOGIN_URL "https://peng-pc.tail941dce.ts.net/login"
// #define STOCK_URL "http://<your-server-ip>:5000/stock"
#define TAG "JWT_DEMO"

void app_main(void) {
    char *token = jwt_load_from_nvs();
    if (!token) {
        ESP_LOGI(TAG, "No token found. Logging in...");

        const char *post_data =
            "{\"username\":\"esp32\", \"password\":\"supersecret\"}";
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
            token = strdup(jwt);

            cJSON_Delete(root);
            free(buf);
        } else {
            ESP_LOGE(TAG, "Login failed: %s", esp_err_to_name(err));
        }
        esp_http_client_cleanup(client);
    }

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
    free(token);
}
