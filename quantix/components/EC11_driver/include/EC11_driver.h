#ifndef __EC11_DRIVER_H
#define __EC11_DRIVER_H

#define PIN_BUTTON 9
#define PIN_ENCODER_A 10
#define PIN_ENCODER_B 11

#define RISING_EDGE 0
#define FALLING_EDGE 1

#define DEBOUNCE_TIME_US 1000

#define BUTTON_PRESSED_BIT BIT0
#define ENCODER_ROTATED_BIT BIT1
#define ENCODER_ROTATED_DIR_BIT BIT2
#define NOTIFY_BIT BIT3

extern EventGroupHandle_t input_event_group;

void setup_gpio(void);

#endif