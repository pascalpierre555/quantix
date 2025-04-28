#include <stdio.h>

#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_log.h"

#define PIN_NUM_MISO  -1  // 這邊如果不用 MISO，設 -1
#define PIN_NUM_MOSI  11  // 記得換成你接的 MOSI 腳位
#define PIN_NUM_CLK   12  // CLK 腳位
#define PIN_NUM_CS    10  // CS 腳位

static const char *TAG = "SPI_EXAMPLE";

spi_device_handle_t spi_handle;

void spi_master_init(void)
{
    esp_err_t ret;

    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_NUM_MISO,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1, // quad SPI不用，設-1
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096, // 最大傳送大小，可根據需求調
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 10 * 1000 * 1000,    // SPI clock 10MHz
        .mode = 0,                              // SPI mode 0
        .spics_io_num = PIN_NUM_CS,             // CS 腳
        .queue_size = 7,                        // transaction queue size
    };

    // 初始化 SPI 總線
    ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);

    // 加入 SPI device
    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &spi_handle);
    ESP_ERROR_CHECK(ret);
}

void spi_master_send(const uint8_t *data, size_t length)
{
    esp_err_t ret;
    spi_transaction_t t = {
        .length = length * 8,    // 注意這邊是 bit，不是 byte
        .tx_buffer = data,
        .rx_buffer = NULL,       // 如果只是傳送，不接收
    };

    ret = spi_device_transmit(spi_handle, &t);  // 傳送
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "Sent data over SPI");
}

void app_main(void)
{
    spi_master_init();

    const char hello_data[] = "Hello SPI!";
    spi_master_send((const uint8_t *)hello_data, sizeof(hello_data) - 1);
}
