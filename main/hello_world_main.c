#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "esp_err.h"
#include "cJSON.h"


// ================= 配置区 =================

#define JETSON_UART UART_NUM_1

// 与当前实际接线保持一致
#define JETSON_TX_PIN 8
#define JETSON_RX_PIN 10

#define UART_BAUD_RATE 115200
#define UART_BUFFER_SIZE 1024

// 测试阶段：每 10 秒发送一次预测请求
#define TEST_INTERVAL_MS 10000

// 未校时成功时，每 5 秒重新请求一次时间
#define TIME_REQUEST_INTERVAL_MS 5000

// 校时成功后，每 60 秒再同步一次
#define TIME_SYNC_INTERVAL_MS 60000

// =========================================


static bool time_ready = false;
static unsigned int request_id = 1;


// 初始化与 Jetson 通信的 UART
static void jetson_uart_init(void)
{
    uart_config_t config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(
        uart_param_config(JETSON_UART, &config)
    );

    ESP_ERROR_CHECK(
        uart_set_pin(
            JETSON_UART,
            JETSON_TX_PIN,
            JETSON_RX_PIN,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE
        )
    );

    ESP_ERROR_CHECK(
        uart_driver_install(
            JETSON_UART,
            UART_BUFFER_SIZE,
            0,
            0,
            NULL,
            0
        )
    );
}


// 通过 UART 发送一行 JSON
static void send_message(const char *message)
{
    uart_write_bytes(JETSON_UART, message, strlen(message));
}


// 向 Jetson 请求系统时间
static void request_jetson_time(void)
{
    const char *message = "{\"cmd\":\"TIME_REQUEST\"}\n";

    send_message(message);
    printf("已向 Jetson 请求时间\n");
}


// 根据 Jetson 返回的毫秒时间戳设置 ESP 系统时间
static bool set_esp_time(const char *unix_ms_text)
{
    if (unix_ms_text == NULL) {
        return false;
    }

    char *end = NULL;
    long long unix_ms = strtoll(unix_ms_text, &end, 10);

    // 检查是否是完整的数字字符串
    if (end == unix_ms_text || *end != '\0') {
        return false;
    }

    // 简单检查：时间不得早于 2024 年，也不得晚于 2100 年
    if (unix_ms < 1704067200000LL || unix_ms >= 4102444800000LL) {
        return false;
    }

    struct timeval tv = {
        .tv_sec = (time_t)(unix_ms / 1000),
        .tv_usec = (suseconds_t)((unix_ms % 1000) * 1000),
    };

    if (settimeofday(&tv, NULL) != 0) {
        return false;
    }

    return true;
}


// 生成 17 位时间，例如 20260415181120561
static bool get_time_string(char *buffer, size_t buffer_size)
{
    if (!time_ready || buffer_size < 18) {
        return false;
    }

    struct timeval tv;
    struct tm time_info;
    char date_part[15];

    if (gettimeofday(&tv, NULL) != 0) {
        return false;
    }

    if (localtime_r(&tv.tv_sec, &time_info) == NULL) {
        return false;
    }

    // 年月日时分秒：14 位
    if (strftime(
            date_part,
            sizeof(date_part),
            "%Y%m%d%H%M%S",
            &time_info
        ) == 0) {
        return false;
    }

    // 加上 3 位毫秒
    int length = snprintf(
        buffer,
        buffer_size,
        "%s%03ld",
        date_part,
        (long)(tv.tv_usec / 1000)
    );

    return length == 17;
}


// 发送预测请求
static void send_predict_request(void)
{
    char time_text[18];
    char message[128];

    if (!get_time_string(time_text, sizeof(time_text))) {
        printf("时间尚未同步，暂不发送预测请求\n");
        return;
    }

    int length = snprintf(
        message,
        sizeof(message),
        "{\"cmd\":\"PREDICT\",\"id\":%u,\"time\":\"%s\"}\n",
        request_id,
        time_text
    );

    if (length <= 0 || length >= (int)sizeof(message)) {
        printf("预测请求生成失败\n");
        return;
    }

    send_message(message);

    printf("发送预测请求: %s", message);

    request_id++;
}


// 处理 Jetson 发回的一整行 JSON
static void handle_jetson_message(const char *line)
{
    cJSON *root = cJSON_Parse(line);

    if (root == NULL) {
        printf("收到的 Jetson 消息不是有效 JSON: %s\n", line);
        return;
    }

    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");

    // 处理 Jetson 的时间同步回复
    if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "TIME_SYNC") == 0) {
        cJSON *unix_ms = cJSON_GetObjectItemCaseSensitive(root, "unix_ms");

        if (cJSON_IsString(unix_ms) && set_esp_time(unix_ms->valuestring)) {
            time_ready = true;

            char time_text[18];

            if (get_time_string(time_text, sizeof(time_text))) {
                printf("ESP 时间同步成功: %s\n", time_text);
            }
        } else {
            printf("Jetson 返回的时间无效\n");
        }

        cJSON_Delete(root);
        return;
    }

    // 处理预测结果或错误
    cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");

    if (cJSON_IsString(status)) {
        int received_id = cJSON_IsNumber(id) ? id->valueint : -1;

        if (strcmp(status->valuestring, "ok") == 0) {
            cJSON *value = cJSON_GetObjectItemCaseSensitive(
                root,
                "concentration"
            );

            if (cJSON_IsNumber(value)) {
                printf(
                    "任务 %d 预测完成，浓度: %.4f mmol/kg\n",
                    received_id,
                    value->valuedouble
                );
            } else {
                printf("任务 %d 返回结果缺少浓度\n", received_id);
            }

        } else if (strcmp(status->valuestring, "error") == 0) {
            cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "message");

            printf(
                "任务 %d 处理失败: %s\n",
                received_id,
                cJSON_IsString(error) ? error->valuestring : "未知错误"
            );

        } else {
            printf(
                "任务 %d 状态: %s\n",
                received_id,
                status->valuestring
            );
        }
    } else {
        printf("收到 Jetson 消息: %s\n", line);
    }

    cJSON_Delete(root);
}


static void jetson_uart_task(void *arg)
{
    char rx_line[UART_BUFFER_SIZE];
    size_t rx_length = 0;
    bool discard_line = false;

    TickType_t last_time_request = xTaskGetTickCount();
    TickType_t last_predict = xTaskGetTickCount();

    // 启动后先请求一次时间
    request_jetson_time();

    while (1) {
        uint8_t byte;

        // 每次读取一个字节，便于按换行符拼接完整消息
        int received = uart_read_bytes(
            JETSON_UART,
            &byte,
            1,
            pdMS_TO_TICKS(100)
        );

        if (received > 0) {
            if (byte == '\n') {
                if (!discard_line && rx_length > 0) {
                    rx_line[rx_length] = '\0';
                    handle_jetson_message(rx_line);
                }

                rx_length = 0;
                discard_line = false;

            } else if (!discard_line && byte != '\r') {
                if (rx_length < sizeof(rx_line) - 1) {
                    rx_line[rx_length++] = (char)byte;
                } else {
                    // 本行过长，丢弃到下一个换行符
                    discard_line = true;
                    rx_length = 0;
                }
            }
        }

        TickType_t now = xTaskGetTickCount();

        // 未校时成功时频繁请求；成功后定期同步
        TickType_t sync_interval = pdMS_TO_TICKS(
            time_ready
                ? TIME_SYNC_INTERVAL_MS
                : TIME_REQUEST_INTERVAL_MS
        );

        if ((TickType_t)(now - last_time_request) >= sync_interval) {
            request_jetson_time();
            last_time_request = now;
        }

        // 测试阶段：校时成功后，每隔 10 秒发送一次
        if (
            time_ready &&
            (TickType_t)(now - last_predict) >= pdMS_TO_TICKS(TEST_INTERVAL_MS)
        ) {
            send_predict_request();
            last_predict = now;
        }
    }
}


void app_main(void)
{
    // 按北京时间格式化时间：UTC+8
    setenv("TZ", "CST-8", 1);
    tzset();

    jetson_uart_init();

    xTaskCreate(
        jetson_uart_task,
        "jetson_uart",
        4096,
        NULL,
        5,
        NULL
    );
}