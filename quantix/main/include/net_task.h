#ifndef NET_TASK_H
#define NET_TASK_H

#include "esp_err.h"

void http_check_connectivity(void);
void cb_connection_ok(void *pvParameter);
void server_login(void);

#endif // NET_TASK_H