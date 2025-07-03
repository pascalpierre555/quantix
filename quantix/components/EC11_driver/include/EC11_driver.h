#ifndef __EC11_DRIVER_H
#define __EC11_DRIVER_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define PIN_BUTTON 9
#define PIN_ENCODER_A 10
#define PIN_ENCODER_B 11

#define RISING_EDGE 0
#define FALLING_EDGE 1

#define BUTTON_PRESSED_BIT BIT0
#define ENCODER_ROTATED_BIT BIT1
#define ENCODER_ROTATED_DIR_BIT BIT2
#define NOTIFY_BIT BIT3

/**
 * @brief Structure to hold the state and configuration for the EC11 driver.
 */
typedef struct {
    volatile uint64_t last_button_time;    /**< Timestamp of the last valid button press. */
    volatile uint64_t last_encoder_time_a; /**< Timestamp of the last edge on pin A. */
    volatile uint64_t last_encoder_time_b; /**< Timestamp of the last edge on pin B. */
    volatile uint64_t
        sec_last_encoder_time_a; /**< Timestamp of the second-to-last edge on pin A. */
    volatile uint64_t
        sec_last_encoder_time_b;           /**< Timestamp of the second-to-last edge on pin B. */
    volatile uint8_t last_encoder_state_a; /**< Last read level of pin A. */
    volatile uint8_t last_encoder_state_b; /**< Last read level of pin B. */
    TaskHandle_t button_callback;          /**< Task to notify on button press. */
    TaskHandle_t encoder_callback;         /**< Task to notify on encoder rotation. */
} ec11_handler_t;


extern EventGroupHandle_t input_event_group;
extern RTC_DATA_ATTR bool isr_woken;
extern RTC_DATA_ATTR ec11_handler_t ec11_handler;

void ec11_set_button_callback(TaskHandle_t cb);
void ec11_clean_button_callback(void);
void ec11_set_encoder_callback(TaskHandle_t cb);
void ec11_clean_encoder_callback(void);
void ec11Startup(void);

#endif