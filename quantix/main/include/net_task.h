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

extern EventGroupHandle_t net_event_group;

#define NET_WIFI_CONNECTED_BIT BIT0
#define NET_SERVER_CONNECTED_BIT BIT1
#define NET_TOKEN_AVAILABLE_BIT BIT2
#define NET_TIME_AVAILABLE_BIT BIT3
#define NET_GOOGLE_TOKEN_AVAILABLE_BIT BIT4

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