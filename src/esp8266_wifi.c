#include "esp8266_wifi.h"
#include "hw_delay.h"
#include "http_helper.h"
#include "picohttpparser.h"
#include "main.h"

const uint8_t AT_OK[6] = {0x0D, 0x0A, 0x4F, 0x4B, 0x0D, 0x0A};
const uint8_t SEND_OK[11] = {0x0D, 0x0A, 0x53, 0x45, 0x4E, 0x44, 0x20, 0x4F, 0x4B, 0x0D, 0x0A};
const uint8_t IPD_FLAG[5] = {0x2B, 0x49, 0x50, 0x44, 0x2C};

struct ESP8266 esp_module = { 0 };

UART_HandleTypeDef esp_uart;
DMA_HandleTypeDef esp_dma_rx;

static uint8_t uart_buffer[ESP_UART_BUFFER_SIZE];

static uint8_t *ipd_pos;
static struct ESP8266_TCP_PACKET tcp_packet = { 0 };

struct phr_header http_headers[HTTP_MAX_HEADERS];
struct HTTP_REQUEST http_request = { 0 };
struct HTTP_RESPONSE http_response = { 0 };
static char http_buffer[HTTP_MAX_SIZE];

static int connect_sta(void);
static int send_tcp_data();
static size_t get_uart_data_length();
static HAL_StatusTypeDef reset_dma_rx();
static uint8_t *check_uart_flag(uint8_t *flag);
static int send_command(size_t length);
static int configure_esp(void);
static void ESP_InitPins(void);
static HAL_StatusTypeDef ESP_InitUART(void);
static HAL_StatusTypeDef ESP_InitDMA(void);

void wifi_task(void * const arg)
{
    xEventGroupSetBits(eg_task_started, EG_WIFI_TSK_STARTED);

    for (;;)
    {
        if (!esp_module.initialized)
        {
            // Init ESP8266
            ESP_InitPins();
            if (ESP_InitUART() == HAL_ERROR) continue;
            if (ESP_InitDMA() == HAL_ERROR) continue;

            // Configure ESP8266
            if (!configure_esp()) continue;

            esp_module.initialized = 1;
        }
        else
        {
            if (reset_dma_rx() == HAL_ERROR) continue;
            if ((ipd_pos = check_uart_flag(IPD_FLAG)) != NULL)
            {
                if ((tcp_packet.data = strstr(uart_buffer, ":")) != NULL)
                    tcp_packet.data++;

                uint16_t length_str_size = tcp_packet.data - (ipd_pos + 7) - 1;
                char length_str[length_str_size];
                memcpy(length_str, ipd_pos + 7, length_str_size);
                tcp_packet.length = atoi(length_str);
                
                http_response.version = 1;

                http_request.headers_count = HTTP_MAX_HEADERS;
                int res = phr_parse_request(
                    (char *)tcp_packet.data, tcp_packet.length, 
                    &http_request.method, &http_request.method_size, 
                    &http_request.route, &http_request.route_size, &http_request.version, 
                    http_headers, &http_request.headers_count
                );
                if (res < 0)
                {
                    http_response.status = 500;
                    http_build_error_response(
                        http_buffer, &http_response.message,
                        &http_response.message_size, &http_response.head_size,
                        http_response.status
                    );
                }
                else
                {
                    http_response.version = http_request.version;
                    if (!http_validate_route(http_request.route, http_request.route_size))
                    {
                        http_response.status = 404;
                        http_build_error_response(
                            http_buffer, &http_response.message,
                            &http_response.message_size, &http_response.head_size,
                            http_response.status
                        );
                    }
                    else if (!http_validate_method(http_request.method, http_request.method_size))
                    {
                        http_response.status = 405;
                        http_build_error_response(
                            http_buffer, &http_response.message,
                            &http_response.message_size, &http_response.head_size,
                            http_response.status
                        );
                    }
                    else if (http_response.version != 1)
                    {
                        http_response.status = 505;
                        http_build_error_response(
                            http_buffer, &http_response.message,
                            &http_response.message_size, &http_response.head_size,
                            http_response.status
                        );
                    }
                    else
                    {
                        http_response.status = 200;
                        if (memcmp(http_request.method, "GET", http_request.method_size) == 0)
                        {
                            http_build_html_response(
                                http_buffer, &http_response.message,
                                &http_response.message_size, &http_response.head_size,
                                http_request.route
                            );
                        }
                    }
                    tcp_packet.length = http_response.head_size;
                    tcp_packet.data = (uint8_t *)http_buffer;
                    send_tcp_data();

                    tcp_packet.length = http_response.message_size;
                    if (http_response.message == NULL)
                        tcp_packet.data = (uint8_t *)(http_buffer + http_response.head_size);
                    else
                        tcp_packet.data = (uint8_t *)http_response.message;
                    
                    send_tcp_data();
                }
            }
        }
        vTaskDelay(500);
    }
}

static int connect_sta(void)
{
    sprintf(uart_buffer, "AT+CWJAP_CUR=\"%s\",\"%s\"\r\n", esp_module.sta_ssid, esp_module.sta_pass);
    if (!send_command(20 + strlen(esp_module.sta_ssid) + strlen(esp_module.sta_pass))) return 0;

    return 1;
}

static int send_tcp_data()
{
    if (tcp_packet.length <= ESP_MAX_TCP_SIZE)
    {
        sprintf(uart_buffer, "AT+CIPSEND=%d,%d\r\n", tcp_packet.id, tcp_packet.length);
        if (!send_command(15 + NUMBER_LENGTH(tcp_packet.length))) return -1;

        if (reset_dma_rx() == HAL_ERROR) return 0;
        HAL_UART_Transmit(&esp_uart, tcp_packet.data, tcp_packet.length, ESP_UART_DELAY);
        if (check_uart_flag(SEND_OK) == NULL) return -1;
    }
    else
    {
        size_t packets_count = tcp_packet.length / ESP_MAX_TCP_SIZE;
        for (uint16_t i = 0; i < packets_count; i++)
        {
            sprintf(uart_buffer, "AT+CIPSEND=%d,%d\r\n", tcp_packet.id, ESP_MAX_TCP_SIZE);
            if (!send_command(15 + NUMBER_LENGTH(ESP_MAX_TCP_SIZE))) return -1;

            if (reset_dma_rx() == HAL_ERROR) return 0;
            HAL_UART_Transmit(&esp_uart, tcp_packet.data+(i*ESP_MAX_TCP_SIZE), ESP_MAX_TCP_SIZE, ESP_UART_DELAY);
            if (check_uart_flag(SEND_OK) == NULL) return -1;
        }
        if ((packets_count * ESP_MAX_TCP_SIZE) < tcp_packet.length)
        {
            size_t tail = tcp_packet.length - (packets_count * ESP_MAX_TCP_SIZE);
            sprintf(uart_buffer, "AT+CIPSEND=%d,%d\r\n", tcp_packet.id, tail);
            if (!send_command(15 + NUMBER_LENGTH(tail))) return -1;

            if (reset_dma_rx() == HAL_ERROR) return 0;
            HAL_UART_Transmit(&esp_uart, tcp_packet.data+(packets_count * ESP_MAX_TCP_SIZE), tail, ESP_UART_DELAY);
            if (check_uart_flag(SEND_OK) == NULL) return -1;
        }
    }
    
    return tcp_packet.length;
}

static size_t get_uart_data_length()
{
    return ESP_UART_BUFFER_SIZE - __HAL_DMA_GET_COUNTER(&esp_dma_rx);
}

static HAL_StatusTypeDef reset_dma_rx()
{
    if (HAL_UART_DMAStop(&esp_uart) == HAL_ERROR) return HAL_ERROR;
    if (HAL_UART_Receive_DMA(&esp_uart, uart_buffer, ESP_UART_BUFFER_SIZE) == HAL_ERROR) return HAL_ERROR;

    return HAL_OK;
}

static uint8_t *check_uart_flag(uint8_t *flag)
{
    static uint32_t uart_notify;
    const TickType_t xMaxBlockTime = pdMS_TO_TICKS( 200 );

    uart_notify = ulTaskNotifyTake(pdFALSE, xMaxBlockTime);
    if (!uart_notify) return NULL;

    return strstr(uart_buffer, flag);
}

static int send_command(size_t length)
{
    if (reset_dma_rx() == HAL_ERROR) return 0;
    if (HAL_UART_Transmit(&esp_uart, uart_buffer, length, ESP_UART_DELAY) == HAL_ERROR) return 0;
    if (check_uart_flag(AT_OK) == NULL) return 0;

    return 1;
}

static int configure_esp(void)
{
    esp_module.mode = STA_AP;
    esp_module.ap_ssid = "AirC Device";
    esp_module.ap_pass = "314159265";
    esp_module.ap_chl = 5;
    esp_module.ap_enc = WPA2_PSK;
    esp_module.mux = 1;
    esp_module.port = HTTP_SERVER_PORT;

    memcpy(uart_buffer, (uint8_t *)"ATE0\r\n", 6);
    if (!send_command(6)) return 0;

    sprintf(uart_buffer, "AT+CWMODE_CUR=%d\r\n", esp_module.mode);
    if (!send_command(17)) return 0;

    sprintf(uart_buffer, "AT+CWSAP_CUR=\"%s\",\"%s\",%d,%d\r\n", esp_module.ap_ssid, esp_module.ap_pass, esp_module.ap_chl, esp_module.ap_enc);
    if (!send_command(24 + strlen(esp_module.ap_ssid) + strlen(esp_module.ap_pass))) return 0;

    sprintf(uart_buffer, "AT+CIPMUX=%d\r\n", esp_module.mux);
    if (!send_command(13)) return 0;

    sprintf(uart_buffer, "AT+CIPSERVER=1,%d\r\n", esp_module.port);
    if (!send_command(17 + NUMBER_LENGTH(esp_module.port))) return 0;

    return 1;
}

static void ESP_InitPins(void)
{
    /* PINS: TX - PD5, RX - PD6*/
    GPIO_InitTypeDef gpio;
    gpio.Pin = GPIO_PIN_5;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF7_USART2;
    HAL_GPIO_Init(GPIOD, &gpio);

    gpio.Pin = GPIO_PIN_6;
    gpio.Mode = GPIO_MODE_AF_OD;
    HAL_GPIO_Init(GPIOD, &gpio);
}

static HAL_StatusTypeDef ESP_InitUART(void)
{
    /* USART 2 */
    esp_uart.Instance = USART2;
    esp_uart.Init.BaudRate = 115200;
    esp_uart.Init.WordLength = UART_WORDLENGTH_8B;
    esp_uart.Init.StopBits = UART_STOPBITS_1;
    esp_uart.Init.Parity = UART_PARITY_NONE;
    esp_uart.Init.Mode = UART_MODE_TX_RX;
    esp_uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    esp_uart.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&esp_uart) == HAL_ERROR) return HAL_ERROR;

    HAL_NVIC_SetPriority(USART2_IRQn, ESP_INT_PRIO, 0);
    HAL_NVIC_EnableIRQ(USART2_IRQn);
    __HAL_UART_ENABLE_IT(&esp_uart, UART_IT_IDLE);

    return HAL_OK;
}

static HAL_StatusTypeDef ESP_InitDMA(void)
{
    esp_dma_rx.Instance = DMA1_Stream5;
    esp_dma_rx.Init.Channel = DMA_CHANNEL_4;
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

    HAL_NVIC_SetPriority(DMA1_Stream5_IRQn, ESP_INT_PRIO, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream5_IRQn);
    
    if (HAL_UART_Receive_DMA(&esp_uart, uart_buffer, ESP_UART_BUFFER_SIZE) == HAL_ERROR) return HAL_ERROR;

    return HAL_OK;
}

void ESP_UART_IRQHandler(UART_HandleTypeDef *huart)
{
    if (USART2 == huart->Instance)
    {
        configASSERT(wifi_tsk_handle != NULL);

        if(RESET != __HAL_UART_GET_FLAG(huart, UART_FLAG_IDLE))
        {
            __HAL_UART_CLEAR_IDLEFLAG(huart);

            BaseType_t uart_rx_task_woken = pdFALSE;
            vTaskNotifyGiveFromISR(wifi_tsk_handle, &uart_rx_task_woken);
            portYIELD_FROM_ISR(uart_rx_task_woken);
        }
    }     
}