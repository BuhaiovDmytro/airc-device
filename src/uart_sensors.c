#include "uart_sensors.h"

#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "string.h"

#define  MAX_SPEC_BUF_LEN 70
#define  MIN_SPEC_BUF_LEN 47

#define MULTIPLEXER_CH2_SO2_TX 2
#define MULTIPLEXER_CH3_SO2_RX 3
#define MULTIPLEXER_CH4_NO2_TX 4
#define MULTIPLEXER_CH5_NO2_RX 5
#define MULTIPLEXER_CH6_CO_TX  6
#define MULTIPLEXER_CH7_CO_RX  7
#define MULTIPLEXER_CH8_O3_TX  8
#define MULTIPLEXER_CH9_O3_RX  9

#define SPEC_RESPONSE_TIME 1000

uint8_t spec_wake = '\n';
uint8_t spec_get_data = '\r';
uint8_t spec_sleep = 's';

volatile char command[MAX_SPEC_BUF_LEN];

struct SPEC_values SPEC_SO2_values, SPEC_NO2_values, SPEC_CO_values, SPEC_O3_values;

static void USART3_UART_Init(void)
{
    huart3.Instance = USART3;
    huart3.Init.BaudRate = 9600;
    huart3.Init.WordLength = UART_WORDLENGTH_8B;
    huart3.Init.StopBits = UART_STOPBITS_1;
    huart3.Init.Parity = UART_PARITY_NONE;
    huart3.Init.Mode = UART_MODE_TX_RX;
    huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart3.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_HalfDuplex_Init(&huart3) != HAL_OK){}
}

static HAL_StatusTypeDef USART3_DMA_Init(void)
{
    huart3_dma_rx.Instance = DMA1_Stream1;
    huart3_dma_rx.Init.Channel = DMA_CHANNEL_4;
    huart3_dma_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
    huart3_dma_rx.Init.PeriphInc = DMA_PINC_DISABLE;
    huart3_dma_rx.Init.MemInc = DMA_MINC_ENABLE;
    huart3_dma_rx.Init.Mode = DMA_NORMAL;
    huart3_dma_rx.Init.Priority = DMA_PRIORITY_VERY_HIGH;
    huart3_dma_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    huart3_dma_rx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    huart3_dma_rx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;

    if (HAL_DMA_Init(&huart3_dma_rx) == HAL_ERROR) return HAL_ERROR;
    __HAL_LINKDMA(&huart3, hdmarx, huart3_dma_rx);

    HAL_NVIC_SetPriority(DMA1_Stream1_IRQn, 5, 0U);
    HAL_NVIC_EnableIRQ(DMA1_Stream1_IRQn);

    if (HAL_UART_Receive_DMA(&huart3, (unsigned char*)command, MAX_SPEC_BUF_LEN) == HAL_ERROR) return HAL_ERROR;

    return HAL_OK;
}

static void GPIO_Init(void) {
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_5|GPIO_PIN_6, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6|GPIO_PIN_9, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_6|GPIO_PIN_8, GPIO_PIN_RESET);

    GPIO_InitStruct.Pin = GPIO_PIN_13;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_5|GPIO_PIN_6;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_6|GPIO_PIN_9;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_6;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

}

static uint8_t chan_table[16][4] = {
        // s0, s1, s2, s3     channel
        {0,  0,  0,  0}, // 0 HCHO sensor RX
        {1,  0,  0,  0}, // 1 PM dust sensor RX
        {0,  1,  0,  0}, // 2 SO2 spec sensor TX
        {1,  1,  0,  0}, // 3 SO2 spec sensor RX
        {0,  0,  1,  0}, // 4 NO2 spec sensor TX
        {1,  0,  1,  0}, // 5 NO2 spec sensor RX
        {0,  1,  1,  0}, // 6 CO spec sensor  TX
        {1,  1,  1,  0}, // 7 CO spec sensor  RX
        {0,  0,  0,  1}, // 8 O3 spec sensor  TX
        {1,  0,  0,  1}, // 9 O3 spec sensor  RX
        {0,  1,  0,  1}, // 10
        {1,  1,  0,  1}, // 11
        {0,  0,  1,  1}, // 12
        {1,  0,  1,  1}, // 13
        {0,  1,  1,  1}, // 14
        {1,  1,  1,  1}  // 15
};

static void activate_multiplexer_channel(uint8_t channel){
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_6, chan_table[channel][0]);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, chan_table[channel][1]);
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_5, chan_table[channel][2]);
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_6, chan_table[channel][3]);
}

struct SPEC_values* get_SO2(void){
    return &SPEC_SO2_values;
}

struct SPEC_values* get_NO2(void){
    return &SPEC_NO2_values;
}

struct SPEC_values* get_CO(void){
    return &SPEC_CO_values;
}

struct SPEC_values* get_O3(void){
    return &SPEC_O3_values;
}

static HAL_StatusTypeDef reset_dma_rx()
{
    if (HAL_UART_DMAStop(&huart3) == HAL_ERROR) return HAL_ERROR;
    if (HAL_UART_Receive_DMA(&huart3, (unsigned char*)command, MAX_SPEC_BUF_LEN) == HAL_ERROR) return HAL_ERROR;

    return HAL_OK;
}

void multiplexerSetState(uint8_t state)
{
    if (state)
    {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
    }
    else
    {
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);
    }
}


HAL_StatusTypeDef getSPEC(uint8_t tx, uint8_t rx, struct SPEC_values *SPEC_gas_values)
{
    HAL_StatusTypeDef return_value = HAL_OK;

    activate_multiplexer_channel(tx);
    multiplexerSetState(1);
    if (HAL_UART_Transmit_IT(&huart3, &spec_wake, 1) != HAL_OK)
    {
        return_value = HAL_ERROR;
    }
    vTaskDelay((TickType_t)SPEC_RESPONSE_TIME/2);

    if (HAL_UART_Transmit_IT(&huart3, &spec_get_data, 1) != HAL_OK)
    {
        return_value = HAL_ERROR;
    }
    vTaskDelay((TickType_t)SPEC_RESPONSE_TIME/2);

    if (HAL_UART_Transmit_IT(&huart3, &spec_get_data, 1) != HAL_OK)
    {
        return_value = HAL_ERROR;
    }
    vTaskDelay((TickType_t)SPEC_RESPONSE_TIME/2);

    activate_multiplexer_channel(rx);

    if (reset_dma_rx() != HAL_OK)
    {
        return HAL_ERROR;
    }

    vTaskDelay((TickType_t)SPEC_RESPONSE_TIME);

    if(ulTaskNotifyTake(pdTRUE, (TickType_t)SPEC_RESPONSE_TIME) >= MIN_SPEC_BUF_LEN)
    {
        if(command[12] == ',' && command[13] == ' ') {
            char *pToNextValue;

            SPEC_gas_values->specSN = strtoull(command, &pToNextValue, 10);
            SPEC_gas_values->specPPB = strtoul(pToNextValue + 2, &pToNextValue, 10);
            SPEC_gas_values->specTemp = strtoul(pToNextValue + 2, &pToNextValue, 10);
            SPEC_gas_values->specRH = strtoul(pToNextValue + 2, &pToNextValue, 10);
            pToNextValue = strstr(pToNextValue + 2, ", ");
            pToNextValue = strstr(pToNextValue + 2, ", ");
            pToNextValue = strstr(pToNextValue + 2, ", ");
            SPEC_gas_values->specDay = strtoul(pToNextValue + 2, &pToNextValue, 10);
            SPEC_gas_values->specHour = strtoul(pToNextValue + 2, &pToNextValue, 10);
            SPEC_gas_values->specMinute = strtoul(pToNextValue + 2, &pToNextValue, 10);
            SPEC_gas_values->specSecond = strtoul(pToNextValue + 2, NULL, 10);
        }

        memset((void*)command, '\0', MAX_SPEC_BUF_LEN);
    }
    else {
        return_value = HAL_ERROR;
    }

    activate_multiplexer_channel(tx);
    if (HAL_UART_Transmit_IT(&huart3, &spec_sleep, 1) != HAL_OK)
    {
        return_value = HAL_ERROR;
    }
    multiplexerSetState(0);

    return return_value;
}

static void UART_sensors_error_handler(){};

void uart_sensors(void * const arg) {

    /* Notify init task that CO sensor task has been started */
    xEventGroupSetBits(eg_task_started, EG_UART_SENSORS_STARTED);

    GPIO_Init();
    USART3_UART_Init();
    USART3_DMA_Init();

    while (1) {

        if (getSPEC(MULTIPLEXER_CH2_SO2_TX, MULTIPLEXER_CH3_SO2_RX, &SPEC_SO2_values) != HAL_OK){
            UART_sensors_error_handler();
        }
        if (getSPEC(MULTIPLEXER_CH4_NO2_TX, MULTIPLEXER_CH5_NO2_RX, &SPEC_NO2_values) != HAL_OK){
            UART_sensors_error_handler();
        }
        if (getSPEC(MULTIPLEXER_CH6_CO_TX, MULTIPLEXER_CH7_CO_RX, &SPEC_CO_values) != HAL_OK){
            UART_sensors_error_handler();
        }
        if (getSPEC(MULTIPLEXER_CH8_O3_TX, MULTIPLEXER_CH9_O3_RX, &SPEC_O3_values) != HAL_OK){
            UART_sensors_error_handler();
        }

        vTaskDelay(500);
    }
}

void HAL_MspInit(void)
{
__HAL_RCC_SYSCFG_CLK_ENABLE();
__HAL_RCC_PWR_CLK_ENABLE();
}

void HAL_UART_MspInit(UART_HandleTypeDef* huart)
{
GPIO_InitTypeDef GPIO_InitStruct = {0};
if(huart->Instance==USART3)
{
    __HAL_RCC_USART3_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    GPIO_InitStruct.Pin = GPIO_PIN_8;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART3;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

    HAL_NVIC_SetPriority(USART3_IRQn, 5, 0U);
    HAL_NVIC_EnableIRQ(USART3_IRQn);
    __HAL_UART_ENABLE_IT(&huart3, UART_IT_IDLE);
}

}

void HAL_UART_MspDeInit(UART_HandleTypeDef* huart)
{
if(huart->Instance==USART3)
{
    __HAL_RCC_USART3_CLK_DISABLE();
    HAL_GPIO_DeInit(GPIOD, GPIO_PIN_8);
    HAL_NVIC_DisableIRQ(USART3_IRQn);
}
}

void UART_SENSORS_IRQHandler(UART_HandleTypeDef *huart)
{
    if (USART3 == huart->Instance)
    {
        configASSERT(uart_sensors_handle != NULL);

        if(RESET != __HAL_UART_GET_FLAG(huart, UART_FLAG_IDLE))
        {
            __HAL_UART_CLEAR_IDLEFLAG(huart);

            BaseType_t uart_rx_task_woken = pdFALSE;

            uint8_t data_len = MAX_SPEC_BUF_LEN - __HAL_DMA_GET_COUNTER(huart3.hdmarx);
            xTaskNotifyAndQueryFromISR(uart_sensors_handle, data_len,
                                       eSetValueWithOverwrite, NULL, &uart_rx_task_woken);
            portYIELD_FROM_ISR(uart_rx_task_woken);
        }
    }
}