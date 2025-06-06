#ifndef NET_TASK_H
#define NET_TASK_H

#include "esp_err.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef enum { NET_CHECK_OK, NET_CHECK_FAIL } net_check_result_t;

extern TaskHandle_t xServerCheckHandle;
extern TaskHandle_t xServerLoginHandle;
extern TaskHandle_t xUserSettingsHandle;
extern QueueHandle_t net_queue;
extern EventGroupHandle_t net_event_group;

#define NET_WIFI_CONNECTED_BIT BIT0
#define NET_SERVER_CONNECTED_BIT BIT1
#define NET_TOKEN_AVAILABLE_BIT BIT2
#define NET_TIME_AVAILABLE_BIT BIT3
#define NET_GOOGLE_TOKEN_AVAILABLE_BIT BIT4

typedef struct {
    const char *url;                 // 請求的完整 URL
    esp_http_client_method_t method; // HTTP 方法
    const char *post_data;           // POST 資料（可為 NULL）
    bool use_jwt;                    // 是否帶 JWT token
    bool save_to_buffer;             // 是否將回應寫入 buffer
    char *response_buffer;           // 回應 buffer（由呼叫者分配）
    size_t response_buffer_size;     // buffer 大小
    void (*on_finish)(struct net_event_t *event, esp_err_t result); // 完成 callback
    void *user_data;                                                // 使用者自訂資料
} net_event_t;

void netStartup(void *pvParameters);
bool http_check_server_connectivity(esp_http_client_handle_t client);
void cb_connection_ok(void *pvParameter);
void server_login(void *pvParameter);
void statusCheck(void *pvParameters);

// typedef enum {
//     HTTP_CMD_LOGIN,
//     HTTP_CMD_SETTINGS,
//     HTTP_CMD_PING,
//     // ...可擴充
// } http_cmd_t;

// typedef struct {
//     http_cmd_t cmd;
//     char url[128];
//     char method[8]; // "GET" or "POST"
//     char body[256];
//     char auth_header[256];
//     TaskHandle_t notify_task; // 回傳時要通知哪個 task
// } http_task_msg_t;

#endif // NET_TASK_H