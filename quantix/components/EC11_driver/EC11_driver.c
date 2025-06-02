#include "EC11_driver.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "Input";

EventGroupHandle_t input_event_group;

ec11_handler_t ec11_handler;

typedef struct {
    volatile uint64_t last_button_time;
    volatile uint64_t last_encoder_time_a;
    volatile uint64_t last_encoder_time_b;
    volatile uint64_t sec_last_encoder_time_a;
    volatile uint64_t sec_last_encoder_time_b;
    volatile uint8_t last_encoder_state_a;
    volatile uint8_t last_encoder_state_b;
    void (*button_callback)(void);
} ec11_handler_t;

void ec11_init(ec11_handler_t *handler) {
    handler->last_button_time = 0;
    handler->last_encoder_time_a = 0;
    handler->last_encoder_time_b = 0;
    handler->sec_last_encoder_time_a = 0;
    handler->sec_last_encoder_time_b = 0;
    handler->last_encoder_state_a = 0;
    handler->last_encoder_state_b = 0;
    handler->button_callback = NULL;
    input_event_group = xEventGroupCreate();
}

void ec11_set_button_callback(void (*cb)(void)) { ec11_handler.button_callback = cb; }

void ec11_clean_button_callback(void) { ec11_handler.button_callback = NULL; }

void IRAM_ATTR button_isr(void *arg) {
    uint64_t now = esp_timer_get_time();
    if (now - ec11_handler.last_button_time > DEBOUNCE_TIME_US) {
        ec11_handler.last_button_time = now;
        // xEventGroupSetBits(input_event_group, BUTTON_PRESSED_BIT);
        if (ec11_handler.button_callback) {
            ec11_handler.button_callback(); // 呼叫 callback
        }
    }
}

void IRAM_ATTR encoder_isr_a(void *arg) {
    uint64_t now = esp_timer_get_time();
    if (now - ec11_handler.last_encoder_time_a > DEBOUNCE_TIME_US) {
        ec11_handler.sec_last_encoder_time_a = ec11_handler.last_encoder_time_a;
        ec11_handler.last_encoder_state_a = gpio_get_level(PIN_ENCODER_A);
    }
    ec11_handler.last_encoder_time_a = now;
}

void IRAM_ATTR encoder_isr_b(void *arg) {
    uint64_t now = esp_timer_get_time();
    if (now - ec11_handler.last_encoder_time_b > DEBOUNCE_TIME_US) {
        ec11_handler.sec_last_encoder_time_b = ec11_handler.last_encoder_time_b;
        ec11_handler.last_encoder_state_b = gpio_get_level(PIN_ENCODER_B);
    }
    ec11_handler.last_encoder_time_b = now;
}

void setup_gpio(void) {
    ec11_init(&ec11_handler);

    gpio_config_t io_conf = {.intr_type = GPIO_INTR_NEGEDGE,
                             .mode = GPIO_MODE_INPUT,
                             .pull_up_en = 1,
                             .pin_bit_mask = (1ULL << PIN_BUTTON)};
    gpio_config(&io_conf);

    gpio_config_t b_conf = {.intr_type = GPIO_INTR_ANYEDGE,
                            .mode = GPIO_MODE_INPUT,
                            .pull_up_en = 1,
                            .pin_bit_mask = (1ULL << PIN_ENCODER_A) | (1ULL << PIN_ENCODER_B)};
    gpio_config(&b_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_BUTTON, button_isr, NULL);
    gpio_isr_handler_add(PIN_ENCODER_A, encoder_isr_a, NULL);
    gpio_isr_handler_add(PIN_ENCODER_B, encoder_isr_b, NULL);
    xTaskCreate(ec11_task, "ec11_task", 2048, NULL, 5, NULL);
}

void ec11_task(void *pvParameters) {
    ESP_LOGI(TAG, "EC11 task started.");
    for (;;) {
        if ((ec11_handler.last_encoder_state_a & 0x01) &&
            (ec11_handler.last_encoder_state_b & 0x01)) {
            ec11_handler.last_encoder_state_a = 0;
            ec11_handler.last_encoder_state_b = 0;
            if (ec11_handler.sec_last_encoder_time_a > ec11_handler.sec_last_encoder_time_b) {
                xEventGroupSetBits(input_event_group, ENCODER_ROTATED_DIR_BIT);
            } else if (ec11_handler.sec_last_encoder_time_b >
                       ec11_handler.sec_last_encoder_time_a) {
                xEventGroupClearBits(input_event_group, ENCODER_ROTATED_DIR_BIT);
            }
            xEventGroupSetBits(input_event_group, ENCODER_ROTATED_BIT);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
