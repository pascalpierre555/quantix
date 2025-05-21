#ifndef NET_TASK_H
#define NET_TASK_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern TaskHandle_t xStatusCheckHandle;

void netStartup(void *pvParameters);
void http_check_connectivity(void *pvParameters);
void cb_connection_ok(void *pvParameter);
void server_login(void *pvParameter);
void statusCheck(void *pvParameters);

#endif // NET_TASK_H