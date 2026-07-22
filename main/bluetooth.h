#ifndef BLUETOOTH_H
#define BLUETOOTH_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define BLUETOOTH_CONFIG_TIMEOUT_DEFAULT_MS 0U
#define BLUETOOTH_CONFIG_TIMEOUT_FOREVER_MS UINT32_MAX

#ifdef __cplusplus
extern "C" {
#endif

// Compatibilidade com o codigo antigo: abre a janela BLE padrao.
void bluetooth_init(void);

// Abre uma janela BLE temporaria para configuracao do GTW.
esp_err_t bluetooth_config_start(uint32_t timeout_ms);

// Fecha advertising/conexao BLE sem desligar WiFi.
void bluetooth_config_stop(void);

bool bluetooth_config_is_active(void);

// Atualiza a caracteristica de status e notifica o celular, se conectado.
void bluetooth_send_message(const char *message);

// Implementado no main.c: processa o JSON recebido via BLE.
void bt_message_received_callback(const char *message);

// Implementados no main.c: alternam o gateway para modo configuracao BLE.
void bt_client_connected_callback(void);
void bt_client_disconnected_callback(void);

#ifdef __cplusplus
}
#endif

#endif
