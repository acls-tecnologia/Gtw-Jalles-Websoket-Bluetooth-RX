#ifndef FIRMWARE_OTA_H
#define FIRMWARE_OTA_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define FW_VERSION_TEXT_MAX 32
#define FW_OTA_STATE_MAX 16

typedef struct {
    char current[FW_VERSION_TEXT_MAX];
    char previous[FW_VERSION_TEXT_MAX];
    char target[FW_VERSION_TEXT_MAX];
    char state[FW_OTA_STATE_MAX];
    char running_partition[17];
    uint32_t boot_count;
    bool ota_capable;
} firmware_ota_info_t;

esp_err_t firmware_ota_init(void);
const firmware_ota_info_t *firmware_ota_get_info(void);
const char *firmware_ota_current_version(void);

// A imagem nova so vira valida depois do periodo minimo de saude.
esp_err_t firmware_ota_mark_running_valid(void);
esp_err_t firmware_ota_schedule_validation(uint32_t delay_ms);

// Agenda a consulta da versao no backend sem bloquear a task de Wi-Fi.
esp_err_t firmware_ota_check_for_update_async(void);

// Baixa e valida uma imagem. Nao reinicia automaticamente.
esp_err_t firmware_ota_update_https(const char *url, const char *bearer_token, const char *ota_access_key,
                                    const char *expected_version);

// Recebe uma imagem .bin diretamente pelo canal BLE de configuracao.
esp_err_t firmware_ota_ble_begin(uint32_t image_size);
esp_err_t firmware_ota_ble_write(uint32_t offset, const uint8_t *data, size_t length, uint8_t *progress_percent);
esp_err_t firmware_ota_ble_finish(char *received_version, size_t received_version_size);
void firmware_ota_ble_abort(void);
bool firmware_ota_ble_is_active(void);

#endif
