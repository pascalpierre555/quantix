#include "EC11_driver.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "Input";

#define BUTTON_PRESSED_BIT BIT0
#define ENCODER_ROTATED_BIT BIT1
#define ENCODER_ROTATED_DIR_BIT BIT2

EventGroupHandle_t input_event_group;

static bool encoder_rotated;
static int encoder_direction; // 1: CW, -1: CCW
static volatile uint64_t last_button_time;
static volatile uint64_t last_encoder_time_a;
static volatile uint64_t last_encoder_time_b;
static volatile uint64_t sec_last_encoder_time_a;
static volatile uint64_t sec_last_encoder_time_b;
static volatile uint8_t last_encoder_state_a;
static volatile uint8_t last_encoder_state_b;

void IRAM_ATTR button_isr(void *arg) {
    uint64_t now = esp_timer_get_time();
    if (now - last_button_time > DEBOUNCE_TIME_US) {
        last_button_time = now;
        xEventGroupSetBits(input_event_group, BUTTON_PRESSED_BIT);
    }
}

void IRAM_ATTR encoder_isr_a(void *arg) {
    uint64_t now = esp_timer_get_time();
    if (now - last_encoder_time_a > DEBOUNCE_TIME_US) {
        sec_last_encoder_time_a = last_encoder_time_a;
        last_encoder_state_a = gpio_get_level(PIN_ENCODER_A);
    }
    last_encoder_time_a = now;
}

void IRAM_ATTR encoder_isr_b(void *arg) {
    uint64_t now = esp_timer_get_time();
    if (now - last_encoder_time_b > DEBOUNCE_TIME_US) {
        sec_last_encoder_time_b = last_encoder_time_b;
        last_encoder_state_b = gpio_get_level(PIN_ENCODER_B);
    }
    last_encoder_time_b = now;
}

void setup_gpio(void) {
    encoder_rotated = false;
    encoder_direction = 0; // 1: CW, -1: CCW
    last_button_time = 0;
    last_encoder_time_a = 0;
    last_encoder_time_b = 0;
    sec_last_encoder_time_a = 0;
    sec_last_encoder_time_b = 0;
    last_encoder_state_a = 0;
    last_encoder_state_b = 0;

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
}

void ec11_task(void *pvParameters) {
    ESP_LOGI(TAG, "EC11 task started.");
    for (;;) {
        if ((last_encoder_state_a & 0x01) && (last_encoder_state_b & 0x01)) {
            last_encoder_state_a = 0;
            last_encoder_state_b = 0;
            if (sec_last_encoder_time_a > sec_last_encoder_time_b) {
                xEventGroupSetBits(input_event_group, ENCODER_ROTATED_DIR_BIT);
            } else if (sec_last_encoder_time_b > sec_last_encoder_time_a) {
                xEventGroupClearBits(input_event_group, ENCODER_ROTATED_DIR_BIT);
            }
            xEventGroupSetBits(input_event_group, ENCODER_ROTATED_BIT);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
