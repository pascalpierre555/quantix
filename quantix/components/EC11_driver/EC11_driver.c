#include "EC11_driver.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

/** @brief Log tag for this module. */
static const char *TAG = "Input";

/** @brief Task handle for the encoder processing task. */
static TaskHandle_t xEncoderTaskHandle;

/** @brief Global instance of the EC11 handler state, stored in RTC memory. */
RTC_DATA_ATTR ec11_handler_t ec11_handler;

/**
 * @brief Initializes the EC11 handler structure to default values.
 * @param handler Pointer to the handler structure to initialize.
 */
static void ec11_init(ec11_handler_t *handler) {
    handler->last_button_time = 0;
    handler->last_encoder_time_a = 0;
    handler->last_encoder_time_b = 0;
    handler->sec_last_encoder_time_a = 0;
    handler->sec_last_encoder_time_b = 0;
    handler->last_encoder_state_a = gpio_get_level(PIN_ENCODER_A);
    handler->last_encoder_state_b = gpio_get_level(PIN_ENCODER_B);
    handler->button_callback = NULL;
    handler->encoder_callback = NULL;
}

/**
 * @brief Sets the callback task to be notified on a button press.
 * @param cb The handle of the task to notify.
 */
void ec11_set_button_callback(TaskHandle_t cb) { ec11_handler.button_callback = cb; }

/**
 * @brief Clears the currently set button callback.
 */
void ec11_clean_button_callback(void) { ec11_handler.button_callback = NULL; }

/**
 * @brief Sets the callback task to be notified on an encoder rotation.
 * @param cb The handle of the task to notify.
 */
void ec11_set_encoder_callback(TaskHandle_t cb) { ec11_handler.encoder_callback = cb; }

/**
 * @brief Clears the currently set encoder callback.
 */
void ec11_clean_encoder_callback(void) {
    ESP_LOGI(TAG, "Cleaning encoder callback");
    ec11_handler.encoder_callback = NULL;
}

/** @brief Debounce time for encoder pins in microseconds. */
#define DEBOUNCE_TIME_US 1000
/** @brief Debounce time for the button pin in microseconds. */
#define DEBOUNCE_TIME_US_BUTTON 50000

/**
 * @brief ISR for the button press. Notifies the registered callback task.
 * @param arg Unused.
 */
void IRAM_ATTR button_isr(void *arg) {
    uint64_t now = esp_timer_get_time();
    if (now - ec11_handler.last_button_time > DEBOUNCE_TIME_US_BUTTON) {
        ec11_handler.last_button_time = now;
        if (ec11_handler.button_callback) {
            vTaskNotifyGiveFromISR(ec11_handler.button_callback, NULL);
        }
    }
}

/**
 * @brief ISR for encoder pin A. Updates state and notifies the processing task.
 * @param arg Unused.
 */
void IRAM_ATTR encoder_isr_a(void *arg) {
    uint64_t now = esp_timer_get_time();
    if (now - ec11_handler.last_encoder_time_a > DEBOUNCE_TIME_US) {
        ec11_handler.sec_last_encoder_time_a = ec11_handler.last_encoder_time_a;
        ec11_handler.last_encoder_state_a = gpio_get_level(PIN_ENCODER_A);
        ec11_handler.last_encoder_time_a = now;
        vTaskNotifyGiveFromISR(xEncoderTaskHandle, NULL);
    }
}

/**
 * @brief ISR for encoder pin B. Updates state and notifies the processing task.
 * @param arg Unused.
 */
void IRAM_ATTR encoder_isr_b(void *arg) {
    uint64_t now = esp_timer_get_time();
    if (now - ec11_handler.last_encoder_time_b > DEBOUNCE_TIME_US) {
        ec11_handler.sec_last_encoder_time_b = ec11_handler.last_encoder_time_b;
        ec11_handler.last_encoder_state_b = gpio_get_level(PIN_ENCODER_B);
        ec11_handler.last_encoder_time_b = now;
        vTaskNotifyGiveFromISR(xEncoderTaskHandle, NULL);
    }
}

/**
 * @brief Task to process encoder rotation events.
 *
 * This task waits for notifications from the encoder ISRs. It decodes the rotation
 * direction based on the sequence of pin changes and notifies the registered callback task.
 * The logic detects a full detent (both pins high) and then compares the timestamps
 * of the previous edges on each pin to determine direction.
 *
 * @param pvParameters Unused.
 */
void ec11_task(void *pvParameters) {
    for (;;) {
        // Wait for a notification from either encoder ISR.
        xTaskNotifyWait(pdFALSE, pdTRUE, NULL, portMAX_DELAY);

        // A full step/detent is detected when both pins are high.
        if ((ec11_handler.last_encoder_state_a & 0x01) &&
            (ec11_handler.last_encoder_state_b & 0x01)) {

            // Reset states to prevent re-triggering on the same detent.
            ec11_handler.last_encoder_state_a = 0;
            ec11_handler.last_encoder_state_b = 0;

            // Compare timestamps of the previous edges to determine direction.
            if (ec11_handler.sec_last_encoder_time_a > ec11_handler.sec_last_encoder_time_b) {
                if (ec11_handler.encoder_callback)
                    xTaskNotify(ec11_handler.encoder_callback, 1, eSetValueWithOverwrite);
            } else if (ec11_handler.sec_last_encoder_time_b >
                       ec11_handler.sec_last_encoder_time_a) {
                if (ec11_handler.encoder_callback)
                    xTaskNotify(ec11_handler.encoder_callback, 2, eSetValueWithOverwrite);
            }
        }
    }
}

/**
 * @brief Initializes the EC11 driver.
 *
 * This function configures the GPIO pins for the encoder and button,
 * installs the ISR service, and creates the encoder processing task.
 * It should be called once at startup.
 */
void ec11Startup(void) {
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

    if (!isr_woken) {
        ec11_init(&ec11_handler);
    }
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_BUTTON, button_isr, NULL);
    gpio_isr_handler_add(PIN_ENCODER_A, encoder_isr_a, NULL);
    gpio_isr_handler_add(PIN_ENCODER_B, encoder_isr_b, NULL);
    xTaskCreate(ec11_task, "ec11_task", 2048, NULL, 6, &xEncoderTaskHandle);
}