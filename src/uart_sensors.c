//
// Created by dmytro on 16.06.20.
//

#include "uart_sensors.h"

#include <stdint.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "main.h"
#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/mem.h"


#define BUF_LEN                         (1024)
#define ECHO_SERVER_PORT                (11333)
#define MAX_CLIENTS                     (4)
static uint8_t buf[BUF_LEN];



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

    if (HAL_HalfDuplex_Init(&huart3) == HAL_OK)
    {
        strncpy(buf, "UART initialized\n", 17);
    }
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

static void Set_CO_RX(void){
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_6, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_5, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_6, GPIO_PIN_RESET);
}

static void Set_CO_TX(void){
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_6, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_5, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_6, GPIO_PIN_RESET);
}

void echo_server(void * const arg)
{
    int listenfd;
    fd_set readfds;
    int maxfd;
    int err;
    int read_len;
    struct sockaddr_in server_addr;
    struct sockaddr_storage client_addr;
    socklen_t client_len;
    struct netif *netif = (struct netif *)arg;

    memset(&server_addr, 0, sizeof(server_addr));

    /* Notify init task that echo server task has been started */
    xEventGroupSetBits(eg_task_started, EG_ECHO_SERVER_STARTED);

    listenfd = lwip_socket(AF_INET, SOCK_STREAM, 0);
    LWIP_ASSERT("echo_server(): Socket create failed", listenfd >= 0);

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = PP_HTONL(INADDR_ANY);
    server_addr.sin_port = lwip_htons(ECHO_SERVER_PORT);

    err = lwip_bind(listenfd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (err < 0) {
        LWIP_ASSERT("echo_server(): Socket bind failed", 0);
    }

    err = lwip_listen(listenfd, MAX_CLIENTS);
    if (err < 0) {
        LWIP_ASSERT("echo_server(): Socket listen failed", 0);
    }

    FD_ZERO(&readfds);
    FD_SET(listenfd, &readfds);
    maxfd = listenfd;

    for (;;)
    {
        err = select((maxfd + 1), &readfds, NULL, NULL, NULL);
        if (err < 0)
            continue;

        for (int fd = 0; fd <= maxfd; ++fd)
        {
            if (FD_ISSET(fd, &readfds))
            {
                if (fd == listenfd)
                {
                    int clientfd = lwip_accept(
                            listenfd,
                            (struct sockaddr *)&client_addr,
                            &client_len);
                    if (clientfd >= 0)
                    {
                        FD_SET(clientfd, &readfds);
                        if (clientfd > maxfd)
                        {
                            maxfd = clientfd;
                        }
                    }
                }
                else
                {
                    read_len = strlen(buf);
                    if (read_len <= 0)
                    {
                        lwip_close(fd);
                        FD_CLR(fd, &readfds);
                    }
                    else
                    {
                        /* send echo */
                        lwip_write(fd, buf, read_len);
                    }
                }
            }
        }
    }
}

volatile uint8_t rx;
volatile uint8_t command[64];
volatile uint8_t command_ready = 0;

void CO_sensor(void * const arg) {
    /* Notify init task that CO sensor task has been started */
    xEventGroupSetBits(eg_task_started, EG_CO_SENSOR_STARTED);

    GPIO_Init();
    USART3_UART_Init();

    Set_CO_TX();
    uint8_t commanda = 'c';
    HAL_UART_Transmit_IT(&huart3, &commanda, 1);
    while (HAL_UART_GetState(&huart3) == HAL_UART_STATE_BUSY_TX);
    HAL_Delay(10000);
    HAL_UART_Transmit_IT(&huart3, &commanda, 1);
    while (HAL_UART_GetState(&huart3) == HAL_UART_STATE_BUSY_TX);

    Set_CO_RX();

    /* Start reception once, rest is done in interrupt handler */
    HAL_UART_Receive_IT(&huart3, &rx, 1);
    while (1) {
        if (command_ready) {
            command_ready = 0;
        }
        vTaskDelay(500);
    }
}
    void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
        if (huart->Instance == USART3) {
            static uint8_t cmd[64];
            static uint8_t icmd;
            cmd[icmd] = rx;
            /* Parse received byte for EOL */
            if (rx == '\n') { /* If \r or \n print text */
                /* Terminate string with \0 */
                cmd[icmd] = 0;
                icmd = 0;
                strncpy(command, cmd, sizeof(command));
                command_ready = 1;
            } else if (rx == '\r') { /* Skip \r character */
            } else { /* If regular character, put it into cmd[] */
                cmd[icmd++] = rx;
            }
            /* Restart reception */
            HAL_UART_Receive_IT(&huart3, &rx, 1);
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

        __HAL_RCC_GPIOD_CLK_ENABLE();
        GPIO_InitStruct.Pin = GPIO_PIN_8;
        GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
        GPIO_InitStruct.Pull = GPIO_PULLUP;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF7_USART3;
        HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

        HAL_NVIC_SetPriority(USART3_IRQn, 0, 0);
        HAL_NVIC_EnableIRQ(USART3_IRQn);
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






