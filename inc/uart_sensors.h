#ifndef AIRC_DEVICE_UART_SENSORS_H
#define AIRC_DEVICE_UART_SENSORS_H

#include "stm32f4xx_hal.h"

UART_HandleTypeDef huart3;

void uart_sensors(void * const arg);

#endif //AIRC_DEVICE_UART_SENSORS_H
