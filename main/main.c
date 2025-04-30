#include "EPD_Test.h"
#include <stdlib.h>
#include "Debug.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    EPD_test();
}
