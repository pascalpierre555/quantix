#ifndef WAKEUP_MANAGER_H
#define WAKEUP_MANAGER_H

#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

void wakeup_handler(void);

#endif // WAKEUP_MANAGER_H