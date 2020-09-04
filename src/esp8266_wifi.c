#include <memory.h>
#include <stdio.h>
#include <limits.h>
#include "math.h"
#include "esp8266_wifi.h"
#include "http_helper.h"
#include "config_board.h"
#include "picohttpparser.h"
#include "main.h"

static boxConfig_S device_config = { 0 };

static struct ESP8266 esp_module = { 0 }; // ESP8266 init struct
volatile int esp_server_mode = 0;

UART_HandleTypeDef esp_uart = { 0 };
DMA_HandleTypeDef esp_dma_rx = { 0 };
DMA_HandleTypeDef esp_dma_tx = { 0 };

static size_t uart_data_size = 0;
static uint8_t uart_buffer[ESP_UART_BUFFER_SIZE];

static struct ESP8266_TCP_PACKET tcp_packet = { 0 };
static char tcp_buffer[ESP_MAX_TCP_SIZE];
static char http_buffer[ESP_MAX_TCP_SIZE];

static struct phr_header http_headers[HTTP_MAX_HEADERS];
static struct HTTP_REQUEST http_request = { 0 };
static struct HTTP_RESPONSE http_response = { 0 };

static size_t get_uart_data_length();
static void reset_dma_rx();

static int esp_tcp_send(uint8_t id, size_t size, char *data);
static uint32_t esp_send_data(uint8_t *data, size_t data_size);
static uint32_t esp_start(void);

static char *double_to_str(double x, char *str, size_t str_size);

void esp_rx_task(void * const arg)
{
    char *pos;

    xEventGroupSetBits(eg_task_started, EG_ESP_RX_TSK_STARTED);

    for (;;)
    {
        HAL_UART_DMAStop(&esp_uart);
        HAL_UART_Receive_DMA(&esp_uart, uart_buffer, ESP_UART_BUFFER_SIZE);
        ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
        uart_data_size = ESP_UART_BUFFER_SIZE - __HAL_DMA_GET_COUNTER(&esp_dma_rx);

        if (strstr((char *)uart_buffer, "\r\nOK\r\n") != NULL)
        {
            xTaskNotify(wifi_tsk_handle, ESP_OK, eSetValueWithOverwrite);
        }
        else if (strstr((char *)uart_buffer, "\r\nERROR\r\n") != NULL)
        {
            xTaskNotify(wifi_tsk_handle, ESP_ERROR, eSetValueWithOverwrite);
        }
        else if (strstr((char *)uart_buffer, "\r\nFAIL\r\n") != NULL)
        {
            xTaskNotify(wifi_tsk_handle, ESP_ERROR, eSetValueWithOverwrite);
        }
        else if (strstr((char *)uart_buffer, "\r\nSEND OK\r\n") != NULL)
        {
            xTaskNotify(wifi_tsk_handle, ESP_OK, eSetValueWithOverwrite);
        }
        else if (strstr((char *)uart_buffer, "\r\nSEND FAIL\r\n") != NULL)
        {
            xTaskNotify(wifi_tsk_handle, ESP_ERROR, eSetValueWithOverwrite);
        }
        else if ((pos = strstr((char *)uart_buffer, "+IPD,")) != NULL)
        {
            if (tcp_packet.status == ESP_TCP_CLOSED)
            {
                int res = sscanf(pos, "+IPD,%d,%d:", &tcp_packet.id, &tcp_packet.length);
                if (res == 2)
                {
                    memcpy(tcp_buffer, pos + 8 + NUMBER_LENGTH(tcp_packet.length), tcp_packet.length);
                    tcp_packet.status = ESP_TCP_OPENED; 
                }
            }
        }
    }
}

void wifi_task(void * const arg)
{
    xEventGroupSetBits(eg_task_started, EG_WIFI_TSK_STARTED);

    esp_start();
    
    for (;;)
    {
        if (tcp_packet.status == ESP_TCP_OPENED && tcp_packet.length > 0)
        {
            http_request.headers_count = HTTP_MAX_HEADERS;
            int http_res = phr_parse_request(
                tcp_buffer, tcp_packet.length, 
                &http_request.method, &http_request.method_size, 
                &http_request.route, &http_request.route_size, &http_request.version, 
                http_headers, &http_request.headers_count
            );
            if (http_res < 0)
            {
                http_response.http_content_type = HTTP_HTML;
                http_response.http_status = HTTP_500;
            }
            else
            {
                if ((http_request.body = strstr(tcp_buffer, "\r\n\r\n")) != NULL)
                {
                    http_request.body += 4;
                    http_request.body_size = tcp_packet.length - ((intptr_t)http_request.body - (intptr_t)tcp_buffer);
                }
                http_check_content_type(&http_response, http_headers, http_request.headers_count);
                if (http_response.http_content_type != HTTP_NOT_ALLOWED)
                {
                    http_check_method(&http_response, http_request.method, http_request.method_size);
                    if (http_response.http_method != HTTP_NOT_ALLOWED)
                    {
                        http_check_route(http_headers, http_request.headers_count, &http_response, http_request.route, http_request.route_size, esp_server_mode);
                        if (http_response.route_index < 0) http_response.http_status = HTTP_404;
                        else if (!http_response.availible) http_response.http_status = HTTP_401;
                        else http_response.http_status = HTTP_200;
                    }
                    else http_response.http_status = HTTP_405;
                }
                else
                {
                    http_response.http_content_type = HTTP_HTML;
                    http_response.http_status = HTTP_415;
                }
            }

            http_build_response(tcp_buffer, &http_response);

            if (http_response.head_size > 0)
            {
                tcp_packet.length = http_response.head_size;

                if (esp_tcp_send(tcp_packet.id, tcp_packet.length, tcp_buffer) >= 0)
                {
                    if (http_response.message_size > 0)
                    {
                        tcp_packet.length = http_response.message_size;
                        if (http_response.message == NULL)
                            tcp_packet.data = tcp_buffer + http_response.head_size;
                        else
                            tcp_packet.data = http_response.message;

                        esp_tcp_send(tcp_packet.id, tcp_packet.length, tcp_packet.data);
                    }
                }
            }
            sprintf((char *)uart_buffer, "AT+CIPCLOSE=%d\r\n", tcp_packet.id);
            esp_send_data(uart_buffer, 15);
            tcp_packet.status = ESP_TCP_CLOSED;
        }
        vTaskDelay(500);
    }
}

void esp_server_handler(ESP8266_SERVER_HANDLER handler)
{
    switch (handler)
    {
    case ESP_GET_WIFI_LIST:
        memcpy((char *)uart_buffer, "AT+CWLAP\r\n", 10);
        if (esp_send_data(uart_buffer, 10) == ESP_OK)
        {
            http_response.message_size = uart_data_size - 6;
            http_response.message = http_buffer;
            memcpy(http_response.message, uart_buffer, http_response.message_size);
        }
        else
        {
            http_response.message = "ERROR";
            http_response.message_size = 5;
        }
        break;
    case ESP_GET_DEVICE_CONF:
        //ReadConfig(&device_config);
        http_response.message_size = sprintf(http_buffer,
        "{\"id\":%d,\"type\":\"%s\",\"desc\":\"%s\",\"lat\":%d}",
        2, "AirC_Box", "AirC box", 50);
        http_response.message = http_buffer;
        /*http_response.message_size = sprintf(http_buffer, 
        "{id:%d,type:\"%s\",desc:\"%s\",lat:%0.7f,long:%0.7f,alt:%0.7f,mode:%d,so2_id:%lld,no2_id:%lld,co_id:%lld,o3_id:%lld}",
        device_config.id, device_config.type, device_config.description, device_config.latitude, device_config.longitude,
        device_config.altitude, device_config.working_status, device_config.SO2_specSN, device_config.NO2_specSN, device_config.CO_specSN, device_config.O3_specSN);
        */
        //http_response.message = "{\"id\":3,\"type\":\"AirC_Car\",\"desc\":\"AirC Device\",\"lat\":50.447731,\"long\":30.542721,\"alt\":30.54,\"mode\":1,\"so2_id\":4321,\"no2_id\":1233,\"co_id\":5336,\"o3_id\":7542}";
        //http_response.message_size = strlen(http_response.message);
        break;
    case ESP_CONNECT_WIFI:
        if (http_request.body_size > 0)
        {
            http_get_form_field(&esp_module.sta_ssid, &esp_module.sta_ssid_size, "ssid=", http_request.body, http_request.body_size);
            http_get_form_field(&esp_module.sta_pass, &esp_module.sta_pass_size, "pass=", http_request.body, http_request.body_size);
            
            sprintf((char *)uart_buffer, "AT+CWJAP_DEF=\"%.*s\",\"%.*s\"\r\n", (int)esp_module.sta_ssid_size, esp_module.sta_ssid, (int)esp_module.sta_pass_size, esp_module.sta_pass);
            if (esp_send_data(uart_buffer, 20 + esp_module.sta_ssid_size + esp_module.sta_pass_size) == ESP_OK)
            {
                http_response.message = "OK";
                http_response.message_size = 2;
            }
            else
            {
                http_response.message = "ERROR";
                http_response.message_size = 5;
            }
        }
        else
        {
            http_response.message = "ERROR";
            http_response.message_size = 5;
        }
        break;
    case ESP_WIFI_MODE:
        if (http_request.body_size > 0)
        {
            char *mode;
            size_t mode_size;
            http_get_form_field(&mode, &mode_size, "mode=", http_request.body, http_request.body_size);

            if (memcmp(mode, "on", mode_size) == 0)
            {
                
                http_response.message = "OK";
                http_response.message_size = 2;
            }
            else if (memcmp(mode, "off", mode_size) == 0)
            {
                memcpy(uart_buffer, (uint8_t *)"AT+CWQAP\r\n", 10);
                if (esp_send_data(uart_buffer, 10) == ESP_OK)
                {
                    http_response.message = "OK";
                    http_response.message_size = 2;
                }
                else
                {
                    http_response.message = "ERROR";
                    http_response.message_size = 5;
                }
            }
            else
            {
                http_response.message = "ERROR";
                http_response.message_size = 5;
            }
        }
        else
        {
            http_response.message = "ERROR";
            http_response.message_size = 5;
        }
        break;
    case ESP_CONF_MODE:
        if (http_request.body_size > 0)
        {
            char *mode;
            size_t mode_size;
            http_get_form_field(&mode, &mode_size, "mode=", http_request.body, http_request.body_size);

            if (memcmp(mode, "on", mode_size) == 0)
            {
                esp_server_mode = 1;
                http_response.message = "OK";
                http_response.message_size = 2;
            }
            else if (memcmp(mode, "off", mode_size) == 0)
            {
                esp_server_mode = 0;
                http_response.message = "OK";
                http_response.message_size = 2;
            }
            else
            {
                http_response.message = "ERROR";
                http_response.message_size = 5;
            }
        }
        else
        {
            http_response.message = "ERROR";
            http_response.message_size = 5;
        }
        break;
    
    default:
        break;
    }
}

static int esp_tcp_send(uint8_t id, size_t size, char *data)
{
    if (size <= ESP_MAX_TCP_SIZE)
    {
        sprintf((char *)uart_buffer, "AT+CIPSEND=%d,%d\r\n", id, size);
        if (esp_send_data(uart_buffer, 15 + NUMBER_LENGTH(size)) == ESP_OK);
        else return -1;

        if (esp_send_data((uint8_t *)data, size) == ESP_OK);
        else return -1;
    }
    else
    {
        size_t packets_count = size / ESP_MAX_TCP_SIZE;
        for (uint16_t i = 0; i < packets_count; i++)
        {
            sprintf((char *)uart_buffer, "AT+CIPSEND=%d,%d\r\n", id, ESP_MAX_TCP_SIZE);
            if (esp_send_data(uart_buffer, 15 + NUMBER_LENGTH(ESP_MAX_TCP_SIZE)) == ESP_OK);
            else return -1;

            if (esp_send_data((uint8_t *)(data+(i*ESP_MAX_TCP_SIZE)), ESP_MAX_TCP_SIZE) == ESP_OK);
            else return -1;
        }
        if ((packets_count * ESP_MAX_TCP_SIZE) < size)
        {
            size_t tail = size - (packets_count * ESP_MAX_TCP_SIZE);
            sprintf((char *)uart_buffer, "AT+CIPSEND=%d,%d\r\n", id, tail);
            if (esp_send_data(uart_buffer, 15 + NUMBER_LENGTH(tail)) == ESP_OK);
            else return -1;

            if (esp_send_data((uint8_t *)(data+(packets_count * ESP_MAX_TCP_SIZE)), tail) == ESP_OK);
            else return -1;
        }
    }
    
    return size;
}

static uint32_t esp_send_data(uint8_t *data, size_t data_size)
{
    uint32_t status = ESP_ERROR;
    HAL_UART_Transmit_DMA(&esp_uart, data, data_size);

    xTaskNotifyStateClear(wifi_tsk_handle);
    status = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    
    return status;
}

static uint32_t esp_start(void)
{
    esp_module.ap_ssid = "AirC Device";
    esp_module.ap_pass = "314159265";
    esp_module.ap_chl = 1;
    esp_module.ap_enc = WPA2_PSK;

    //ReadConfig(&device_config);

    // Disable AT commands echo
    memcpy(uart_buffer, (uint8_t *)"ATE0\r\n", 6);
    if (esp_send_data(uart_buffer, 6) == ESP_OK);
    else return 0;

    // Set soft AP + station mode
    memcpy(uart_buffer, (uint8_t *)"AT+CWMODE_DEF=3\r\n", 17);
    if (esp_send_data(uart_buffer, 17) == ESP_OK);
    else return 0;

    // Set auto connect to saved wifi network
    memcpy(uart_buffer, (uint8_t *)"AT+CWAUTOCONN=1\r\n", 17);
    if (esp_send_data(uart_buffer, 17) == ESP_OK);
    else return 0;

    memcpy(uart_buffer, (uint8_t *)"AT+CWJAP_DEF?\r\n", 15);
    if (esp_send_data(uart_buffer, 15) == ESP_OK);

    // Set IP for soft AP
    memcpy(uart_buffer, (uint8_t *)("AT+CIPAP_DEF=\"" ESP_SERVER_HOST "\"\r\n"), 17 + strlen(ESP_SERVER_HOST));
    if (esp_send_data(uart_buffer, 17 + strlen(ESP_SERVER_HOST)) == ESP_OK);
    else return 0;

    // Configure soft AP
    sprintf((char *)uart_buffer, "AT+CWSAP_DEF=\"%s\",\"%s\",%d,%d\r\n", 
        esp_module.ap_ssid, esp_module.ap_pass, 
        esp_module.ap_chl, esp_module.ap_enc);
    if (esp_send_data(uart_buffer, 24 + strlen(esp_module.ap_ssid) + strlen(esp_module.ap_pass)) == ESP_OK);
    else return 0;

    // Allow multiple TCP connections
    memcpy(uart_buffer, "AT+CIPMUX=1\r\n", 13);
    if (esp_send_data(uart_buffer, 13) == ESP_OK);
    else return 0;

    // Start server
    sprintf((char *)uart_buffer, "AT+CIPSERVER=1,%d\r\n", HTTP_SERVER_PORT);
    if (esp_send_data(uart_buffer, 17 + NUMBER_LENGTH(HTTP_SERVER_PORT)) == ESP_OK);
    else return 0;

    return 1;
}

void ESP_InitPins(void)
{
    /* PINS: TX - PC6, RX - PC7*/
    GPIO_InitTypeDef gpio;
    gpio.Pin = GPIO_PIN_6;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF8_USART6;
    HAL_GPIO_Init(GPIOC, &gpio);

    gpio.Pin = GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_AF_OD;
    HAL_GPIO_Init(GPIOC, &gpio);
}

HAL_StatusTypeDef ESP_InitUART(void)
{
    /* USART 6 */
    esp_uart.Instance = USART6;
    esp_uart.Init.BaudRate = 115200;
    esp_uart.Init.WordLength = UART_WORDLENGTH_8B;
    esp_uart.Init.StopBits = UART_STOPBITS_1;
    esp_uart.Init.Parity = UART_PARITY_NONE;
    esp_uart.Init.Mode = UART_MODE_TX_RX;
    esp_uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    esp_uart.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&esp_uart) == HAL_ERROR) return HAL_ERROR;

    HAL_NVIC_SetPriority(USART6_IRQn, ESP_INT_PRIO, 0);
    HAL_NVIC_EnableIRQ(USART6_IRQn);
    __HAL_UART_ENABLE_IT(&esp_uart, UART_IT_IDLE);

    return HAL_OK;
}

HAL_StatusTypeDef ESP_InitDMA(void)
{
    esp_dma_tx.Instance = DMA2_Stream6;
    esp_dma_tx.Init.Channel = DMA_CHANNEL_5;
    esp_dma_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
    esp_dma_tx.Init.PeriphInc = DMA_PINC_DISABLE;
    esp_dma_tx.Init.MemInc = DMA_MINC_ENABLE;
    esp_dma_tx.Init.Mode = DMA_NORMAL;
    esp_dma_tx.Init.Priority = DMA_PRIORITY_VERY_HIGH;
    esp_dma_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    esp_dma_tx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    esp_dma_tx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;

    if (HAL_DMA_Init(&esp_dma_tx) == HAL_ERROR) return HAL_ERROR;
    __HAL_LINKDMA(&esp_uart, hdmatx, esp_dma_tx);

    esp_dma_rx.Instance = DMA2_Stream1;
    esp_dma_rx.Init.Channel = DMA_CHANNEL_5;
    esp_dma_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
    esp_dma_rx.Init.PeriphInc = DMA_PINC_DISABLE;
    esp_dma_rx.Init.MemInc = DMA_MINC_ENABLE;
    esp_dma_rx.Init.Mode = DMA_NORMAL;
    esp_dma_rx.Init.Priority = DMA_PRIORITY_VERY_HIGH;
    esp_dma_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    esp_dma_rx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    esp_dma_rx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;

    if (HAL_DMA_Init(&esp_dma_rx) == HAL_ERROR) return HAL_ERROR;
    __HAL_LINKDMA(&esp_uart, hdmarx, esp_dma_rx);

    return HAL_OK;
}

void ESP_UART_IRQHandler(UART_HandleTypeDef *huart)
{
    if (USART6 == huart->Instance)
    {
        configASSERT(esp_rx_tsk_handle != NULL);

        if(RESET != __HAL_UART_GET_FLAG(huart, UART_FLAG_IDLE))
        {
            __HAL_UART_CLEAR_IDLEFLAG(huart);

            BaseType_t task_woken = pdFALSE;
            vTaskNotifyGiveFromISR(esp_rx_tsk_handle, &task_woken);
            portYIELD_FROM_ISR(task_woken);
        }
    }     
}

static char *double_to_str(double x, char *str, size_t str_size)
{
    char *str_end = str + str_size;
    uint16_t decimals;
    int units;
    if (x < 0)
    {
        decimals = (uint16_t)(x * -100) % 100;
        units = (int)(-1 * x);
    }
    else
    {
        decimals = (uint16_t)(x * 100) % 100;
        units = (int)x;
    }

    *--str_end = (char)(decimals % 10) + '0';
    decimals /= 10;
    *--str_end = (char)(decimals % 10) + '0';
    *--str_end = '.';

    while (units > 0)
    {
        *--str_end = (char)(units % 10) + '0';
        units /= 10;
    }

    if (x < 0) *--str_end = '-';
    
    return str_end;
}