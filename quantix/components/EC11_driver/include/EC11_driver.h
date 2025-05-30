#ifndef __EC11_DRIVER_H
#define __EC11_DRIVER_H

#define PIN_BUTTON 9
#define PIN_ENCODER_A 10
#define PIN_ENCODER_B 11

#define RISING_EDGE 0
#define FALLING_EDGE 1

#define DEBOUNCE_TIME_US 1000

void setup_gpio(void);

#endif