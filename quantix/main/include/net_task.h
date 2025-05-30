#ifndef NET_TASK_H
#define NET_TASK_H

#include "esp_err.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef enum { NET_CHECK_OK, NET_CHECK_FAIL } net_check_result_t;

extern TaskHandle_t xServerCheckHandle;

extern EventGroupHandle_t net_event_group;

#define NET_WIFI_CONNECTED_BIT BIT0
#define NET_SERVER_CONNECTED_BIT BIT1
#define NET_TOKEN_AVAILABLE_BIT BIT2

void netStartup(void *pvParameters);
bool http_check_server_connectivity(esp_http_client_handle_t client);
void cb_connection_ok(void *pvParameter);
void server_login(void *pvParameter);
void statusCheck(void *pvParameters);

#endif // NET_TASK_H