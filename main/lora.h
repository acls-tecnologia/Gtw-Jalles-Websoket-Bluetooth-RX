#pragma once
#include "driver/uart.h"
#include "esp_err.h"
#include <stdint.h>

typedef struct {
    uart_port_t uart_num;
    int tx_pin;
    int rx_pin;
    int m0_pin;
    int m1_pin;
    int rst_pin; // -1 se não usa

    // UART do ESP (não confundir com air rate)
    int uart_baudrate; // ex: 9600

    // Config do E32 (6 bytes)
    uint8_t head;    // 0xC0 (write permanent) ou 0xC2 (write temp)
    uint8_t addh;    // ex: 0x00
    uint8_t addl;    // ex: 0x01
    uint8_t speed;   // ex: 0x1A (0x18 | 0x02)
    uint8_t channel; // ex: 0x1
    uint8_t option;  // ex: 0x64 (0x40|0x20|0x04)
} lora_e32_config_t;

esp_err_t lora_e32_init(const lora_e32_config_t *cfg);
esp_err_t lora_e32_reinit(void);
esp_err_t lora_e32_apply_cfg(void); // grava os 6 bytes no rádio (com delays)
esp_err_t lora_e32_apply_cfg_temp(void);

int lora_e32_send_raw(const uint8_t *data, int len);
int lora_e32_receive_raw(uint8_t *out, int maxlen, int timeout_ms);
void lora_e32_flush_rx(void);
