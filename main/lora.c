#include "lora.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "LORA_E32";

static lora_e32_config_t g_cfg;
static bool g_uart_installed = false;
static uint8_t g_rx_stream[512];
static size_t g_rx_stream_len = 0;
static int64_t g_rx_stream_last_byte_us = 0;
static int64_t g_rx_last_timeout_buffer_log_us = 0;
static uint32_t s_rx_calls = 0;
static uint32_t s_rx_ok = 0;
static uint32_t s_rx_timeout = 0;
static uint32_t s_rx_bytes = 0;
static uint32_t s_rx_drop_noise = 0;
static uint32_t s_rx_crc_fail = 0;
static uint32_t s_rx_overflow = 0;

#define RX_STALE_BUFFER_MS 1500
#define RX_TIMEOUT_BUFFER_LOG_MS 5000
#define LORA_STREAM_HEADER_LEN 13
#define LORA_STREAM_LEN_OFFSET 9
#define LORA_STREAM_MAX_PAYLOAD 64
#define LORA_STREAM_CRC_LEN 2

static void log_rx_stats(const char *motivo) {
    ESP_LOGW(TAG,
             "RX_STATS motivo=%s calls=%lu ok=%lu timeout=%lu bytes=%lu noise_drop=%lu crc_fail=%lu overflow=%lu buffer=%u",
             motivo,
             (unsigned long)s_rx_calls,
             (unsigned long)s_rx_ok,
             (unsigned long)s_rx_timeout,
             (unsigned long)s_rx_bytes,
             (unsigned long)s_rx_drop_noise,
             (unsigned long)s_rx_crc_fail,
             (unsigned long)s_rx_overflow,
             (unsigned)g_rx_stream_len);
}

static uint16_t stream_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static void stream_drop(size_t count) {
    if (count >= g_rx_stream_len) {
        g_rx_stream_len = 0;
        g_rx_stream_last_byte_us = 0;
        return;
    }
    memmove(g_rx_stream, g_rx_stream + count, g_rx_stream_len - count);
    g_rx_stream_len -= count;
    if (g_rx_stream_len == 0)
        g_rx_stream_last_byte_us = 0;
}

static void set_mode_cfg(void) {
    gpio_set_level(g_cfg.m0_pin, 1);
    gpio_set_level(g_cfg.m1_pin, 1);
}

static void set_mode_normal(void) {
    gpio_set_level(g_cfg.m0_pin, 0);
    gpio_set_level(g_cfg.m1_pin, 0);
}

static void hw_reset_if_any(void) {
    if (g_cfg.rst_pin < 0)
        return;

    gpio_set_direction(g_cfg.rst_pin, GPIO_MODE_OUTPUT);
    gpio_set_level(g_cfg.rst_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(g_cfg.rst_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
}

esp_err_t lora_e32_init(const lora_e32_config_t *cfg) {
    if (!cfg)
        return ESP_ERR_INVALID_ARG;
    g_cfg = *cfg;

    // GPIO M0/M1
    gpio_set_direction(g_cfg.m0_pin, GPIO_MODE_OUTPUT);
    gpio_set_direction(g_cfg.m1_pin, GPIO_MODE_OUTPUT);

    // Garante modo NORMAL primeiro
    set_mode_normal();
    vTaskDelay(pdMS_TO_TICKS(50));

    // UART (ordem igual ao seu teste que funcionou)
    uart_config_t uc = {.baud_rate = g_cfg.uart_baudrate,
                        .data_bits = UART_DATA_8_BITS,
                        .parity = UART_PARITY_DISABLE,
                        .stop_bits = UART_STOP_BITS_1,
                        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE};

    if (!g_uart_installed) {
        ESP_ERROR_CHECK(uart_driver_install(g_cfg.uart_num, 2048, 2048, 0, NULL, 0));
        g_uart_installed = true;
    }

    ESP_ERROR_CHECK(uart_param_config(g_cfg.uart_num, &uc));
    ESP_ERROR_CHECK(uart_set_pin(g_cfg.uart_num, g_cfg.tx_pin, g_cfg.rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // Reset (se existir)
    hw_reset_if_any();

    ESP_LOGI(TAG, "init ok (uart=%d tx=%d rx=%d m0=%d m1=%d rst=%d baud=%d)", g_cfg.uart_num, g_cfg.tx_pin,
             g_cfg.rx_pin, g_cfg.m0_pin, g_cfg.m1_pin, g_cfg.rst_pin, g_cfg.uart_baudrate);

    return ESP_OK;
}

esp_err_t lora_e32_reinit(void) {
    return lora_e32_init(&g_cfg);
}

static bool cfg_response_matches(const uint8_t *resp, int len, const uint8_t *cmd) {
    for (int offset = 0; offset + 6 <= len; offset++) {
        uint8_t response_head = resp[offset];
        bool known_head = response_head == 0xC0 || response_head == 0xC1 || response_head == 0xC2;

        bool same_address = resp[offset + 1] == cmd[1] && resp[offset + 2] == cmd[2];
        bool same_radio = resp[offset + 4] == cmd[4] && resp[offset + 5] == cmd[5];
        bool speed_matches = resp[offset + 3] == cmd[3];

        // A rede instalada usa 0x18. Algumas revisoes do E32 mantem/retornam
        // 0x1A; isso nao deve bloquear a UART nem a recepcao dos frames legados.
        if (cmd[3] == 0x18 && resp[offset + 3] == 0x1A) {
            speed_matches = true;
            ESP_LOGW(TAG, "E32 retornou SPED=0x1A para a configuracao legada 0x18; mantendo radio disponivel");
        }

        if (known_head && same_address && speed_matches && same_radio)
            return true;
    }

    return false;
}

static esp_err_t lora_e32_apply_cfg_with_head(uint8_t head) {
    if (!g_uart_installed) {
        ESP_LOGE(TAG, "UART do E32 nao esta inicializada");
        return ESP_ERR_INVALID_STATE;
    }

    g_rx_stream_len = 0;
    esp_err_t result = ESP_OK;

    set_mode_cfg();
    vTaskDelay(pdMS_TO_TICKS(500));

    uint8_t cmd[6] = {head, g_cfg.addh, g_cfg.addl, g_cfg.speed, g_cfg.channel, g_cfg.option};

    result = uart_flush_input(g_cfg.uart_num);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao limpar RX antes da configuracao: %s", esp_err_to_name(result));
        goto restore_normal_mode;
    }

    int written = uart_write_bytes(g_cfg.uart_num, (const char *)cmd, sizeof(cmd));
    if (written != (int)sizeof(cmd)) {
        ESP_LOGE(TAG, "Falha ao enviar configuracao ao E32: escrito=%d esperado=%u", written,
                 (unsigned)sizeof(cmd));
        result = written < 0 ? ESP_FAIL : ESP_ERR_INVALID_SIZE;
        goto restore_normal_mode;
    }

    result = uart_wait_tx_done(g_cfg.uart_num, pdMS_TO_TICKS(500));
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Timeout/falha ao transmitir configuracao: %s", esp_err_to_name(result));
        goto restore_normal_mode;
    }

    vTaskDelay(pdMS_TO_TICKS(300));

    uint8_t resp[16];
    int r = uart_read_bytes(g_cfg.uart_num, resp, sizeof(resp), pdMS_TO_TICKS(500));
    if (r > 0) {
        char line[128];
        int p = 0;
        p += snprintf(line + p, sizeof(line) - p, "resp[%d]:", r);
        for (int i = 0; i < r; i++)
            p += snprintf(line + p, sizeof(line) - p, " %02X", resp[i]);
        ESP_LOGI(TAG, "%s", line);

        if (!cfg_response_matches(resp, r, cmd)) {
            ESP_LOGE(TAG,
                     "Resposta do E32 nao confirma os parametros enviados (esperado: %02X %02X %02X %02X %02X)",
                     cmd[1], cmd[2], cmd[3], cmd[4], cmd[5]);
            result = ESP_ERR_INVALID_RESPONSE;
        }
    } else if (r == 0) {
        ESP_LOGE(TAG, "E32 nao respondeu a configuracao");
        result = ESP_ERR_TIMEOUT;
    } else {
        ESP_LOGE(TAG, "Falha ao ler resposta da configuracao do E32");
        result = ESP_FAIL;
    }

restore_normal_mode:
    set_mode_normal();
    vTaskDelay(pdMS_TO_TICKS(500));

    if (result == ESP_OK) {
        ESP_LOGI(TAG, "radio cfg confirmed: %02X %02X %02X %02X %02X %02X", cmd[0], cmd[1], cmd[2], cmd[3],
                 cmd[4], cmd[5]);
    }

    return result;
}

esp_err_t lora_e32_apply_cfg(void) {
    return lora_e32_apply_cfg_with_head(g_cfg.head);
}

esp_err_t lora_e32_apply_cfg_temp(void) {
    return lora_e32_apply_cfg_with_head(0xC2);
}

int lora_e32_send_raw(const uint8_t *data, int len) {
    if (!data || len <= 0)
        return 0;

    int total = 0;
    while (total < len) {
        int written = uart_write_bytes(g_cfg.uart_num, (const char *)data + total, len - total);
        if (written <= 0)
            return (total > 0) ? total : written;
        total += written;
    }

    if (uart_wait_tx_done(g_cfg.uart_num, pdMS_TO_TICKS(1500)) != ESP_OK)
        return -1;

    return total;
}

void lora_e32_flush_rx(void) {
    if (g_uart_installed) {
        uart_flush_input(g_cfg.uart_num);
    }

    g_rx_stream_len = 0;
    g_rx_stream_last_byte_us = 0;
}

int lora_e32_receive_raw(uint8_t *out, int maxlen, int timeout_ms) {
    if (!out || maxlen <= 0)
        return -1;

    s_rx_calls++;
    int64_t deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000);

    while (esp_timer_get_time() < deadline_us || g_rx_stream_len >= LORA_STREAM_HEADER_LEN) {
        while (g_rx_stream_len > 0 && g_rx_stream[0] != 0xAA) {
            uint8_t dropped = g_rx_stream[0];
            stream_drop(1);
            s_rx_drop_noise++;
            if ((s_rx_drop_noise % 32U) == 1U) {
                ESP_LOGW(TAG, "RX byte sem preamble drop=0x%02X total_noise=%lu", dropped,
                         (unsigned long)s_rx_drop_noise);
                log_rx_stats("noise");
            }
        }

        if (g_rx_stream_len >= LORA_STREAM_HEADER_LEN) {
            uint8_t payload_len = g_rx_stream[LORA_STREAM_LEN_OFFSET];
            size_t frame_len = LORA_STREAM_HEADER_LEN + (size_t)payload_len + LORA_STREAM_CRC_LEN;

            if (payload_len > LORA_STREAM_MAX_PAYLOAD || frame_len > (size_t)maxlen) {
                s_rx_drop_noise++;
                ESP_LOGW(TAG, "RX frame len invalido payload=%u frame=%u max=%d", payload_len,
                         (unsigned)frame_len, maxlen);
                stream_drop(1);
                continue;
            }

            if (g_rx_stream_len < frame_len)
                goto read_more;

            uint16_t received_crc = 0;
            memcpy(&received_crc, g_rx_stream + frame_len - sizeof(received_crc), sizeof(received_crc));
            uint16_t calculated_crc = stream_crc16(g_rx_stream, frame_len - sizeof(received_crc));

            if (received_crc == calculated_crc) {
                memset(out, 0, maxlen);
                memcpy(out, g_rx_stream, frame_len);
                stream_drop(frame_len);
                s_rx_ok++;
                if ((s_rx_ok % 100U) == 0U)
                    log_rx_stats("ok_100");
                return (int)frame_len;
            }

            s_rx_crc_fail++;
            ESP_LOGW(TAG, "RX CRC invalido recebido=0x%04X calculado=0x%04X total_crc_fail=%lu", received_crc,
                     calculated_crc, (unsigned long)s_rx_crc_fail);
            log_rx_stats("crc_fail");
            stream_drop(1);
            continue;
        }

read_more:
        ;
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0)
            break;

        int remaining_ms = (int)((remaining_us + 999) / 1000);
        if (remaining_ms > 100)
            remaining_ms = 100;

        uint8_t chunk[128];
        int r = uart_read_bytes(g_cfg.uart_num, chunk, sizeof(chunk), pdMS_TO_TICKS(remaining_ms));

        if (r < 0)
            return r;
        if (r == 0)
            continue;

        if (g_rx_stream_len + (size_t)r > sizeof(g_rx_stream)) {
            size_t excess = g_rx_stream_len + (size_t)r - sizeof(g_rx_stream);
            s_rx_overflow++;
            ESP_LOGW(TAG, "RX stream overflow excess=%u chunk=%d total_overflow=%lu", (unsigned)excess, r,
                     (unsigned long)s_rx_overflow);
            log_rx_stats("overflow");
            stream_drop(excess);
        }

        memcpy(g_rx_stream + g_rx_stream_len, chunk, r);
        g_rx_stream_len += r;
        g_rx_stream_last_byte_us = esp_timer_get_time();
        s_rx_bytes += (uint32_t)r;
    }

    s_rx_timeout++;
    if (g_rx_stream_len > 0) {
        int64_t now_us = esp_timer_get_time();
        int64_t stale_ms = g_rx_stream_last_byte_us > 0 ? (now_us - g_rx_stream_last_byte_us) / 1000 : 0;

        if (stale_ms >= RX_STALE_BUFFER_MS) {
            ESP_LOGW(TAG, "RX buffer incompleto descartado por timeout: bytes=%u stale_ms=%lld",
                     (unsigned)g_rx_stream_len, stale_ms);
            log_rx_stats("stale_buffer_drop");
            stream_drop(g_rx_stream_len);
        } else if ((now_us - g_rx_last_timeout_buffer_log_us) >= (RX_TIMEOUT_BUFFER_LOG_MS * 1000LL)) {
            g_rx_last_timeout_buffer_log_us = now_us;
            log_rx_stats("timeout_com_buffer");
        }
    } else if ((s_rx_timeout % 500U) == 0U) {
        log_rx_stats("timeout_vazio_500");
    }

    return 0;
}
