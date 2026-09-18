#include "firmware_ota.h"

#include "cJSON.h"
#include "config.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FW_NVS_NAMESPACE "fw_meta"
#define FW_NVS_SCHEMA 1
#define FW_AUTH_HEADER_MAX 720
#define FW_OTA_KEY_MAX 257
#define FW_METADATA_BODY_MAX 512

static const char *FW_TAG = "FW_OTA";
static firmware_ota_info_t s_info;
static SemaphoreHandle_t s_update_mutex;
static bool s_initialized;
static bool s_validation_scheduled;
static char s_auth_header[FW_AUTH_HEADER_MAX];
static char s_ota_access_key[FW_OTA_KEY_MAX];
static TaskHandle_t s_check_task;

typedef struct {
    esp_ota_handle_t handle;
    const esp_partition_t *partition;
    uint32_t expected_size;
    uint32_t received_size;
    int last_log_percent;
    bool active;
} ble_ota_session_t;

static ble_ota_session_t s_ble_ota;

typedef struct {
    char body[FW_METADATA_BODY_MAX];
    size_t length;
    bool overflow;
} ota_metadata_response_t;

static const char *image_state_name(esp_ota_img_states_t state) {
    switch (state) {
    case ESP_OTA_IMG_NEW:
        return "new";
    case ESP_OTA_IMG_PENDING_VERIFY:
        return "pending";
    case ESP_OTA_IMG_VALID:
        return "valid";
    case ESP_OTA_IMG_INVALID:
        return "invalid";
    case ESP_OTA_IMG_ABORTED:
        return "aborted";
    default:
        return "active";
    }
}

static esp_err_t nvs_save_text(const char *key, const char *value) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
        return err;
    err = nvs_set_str(handle, key, value ? value : "");
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t set_state(const char *state) {
    esp_err_t err = nvs_save_text("state", state);
    if (err == ESP_OK)
        strlcpy(s_info.state, state, sizeof(s_info.state));
    return err;
}

esp_err_t firmware_ota_init(void) {
    memset(&s_info, 0, sizeof(s_info));
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    strlcpy(s_info.current, (app && app->version[0]) ? app->version : "unknown", sizeof(s_info.current));
    strlcpy(s_info.running_partition, running ? running->label : "unknown", sizeof(s_info.running_partition));
    s_info.ota_capable = running && next && running->address != next->address;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
        return err;

    char stored[FW_VERSION_TEXT_MAX] = {0};
    size_t stored_len = sizeof(stored);
    bool has_stored = nvs_get_str(handle, "current", stored, &stored_len) == ESP_OK && stored[0];

    size_t previous_len = sizeof(s_info.previous);
    if (nvs_get_str(handle, "previous", s_info.previous, &previous_len) != ESP_OK)
        s_info.previous[0] = '\0';
    size_t target_len = sizeof(s_info.target);
    if (nvs_get_str(handle, "target", s_info.target, &target_len) != ESP_OK)
        s_info.target[0] = '\0';

    if (!has_stored) {
        err = nvs_set_str(handle, "current", s_info.current);
        strlcpy(s_info.state, s_info.ota_capable ? "active" : "no_ota", sizeof(s_info.state));
    } else if (strcmp(stored, s_info.current) != 0) {
        strlcpy(s_info.previous, stored, sizeof(s_info.previous));
        err = nvs_set_str(handle, "previous", stored);
        if (err == ESP_OK)
            err = nvs_set_str(handle, "current", s_info.current);
        if (err == ESP_OK)
            err = nvs_set_u32(handle, "boot_count", 0);
        strlcpy(s_info.state, "pending", sizeof(s_info.state));
    } else {
        size_t state_len = sizeof(s_info.state);
        if (nvs_get_str(handle, "state", s_info.state, &state_len) != ESP_OK)
            strlcpy(s_info.state, s_info.ota_capable ? "active" : "no_ota", sizeof(s_info.state));
    }

    esp_ota_img_states_t image_state = ESP_OTA_IMG_UNDEFINED;
    if (running && esp_ota_get_state_partition(running, &image_state) == ESP_OK)
        strlcpy(s_info.state, image_state_name(image_state), sizeof(s_info.state));

    if (err == ESP_OK) {
        esp_err_t count_err = nvs_get_u32(handle, "boot_count", &s_info.boot_count);
        if (count_err == ESP_ERR_NVS_NOT_FOUND)
            s_info.boot_count = 0;
        else if (count_err != ESP_OK)
            err = count_err;
    }
    if (err == ESP_OK)
        err = nvs_set_u32(handle, "boot_count", ++s_info.boot_count);
    if (err == ESP_OK)
        err = nvs_set_u8(handle, "schema", FW_NVS_SCHEMA);
    if (err == ESP_OK)
        err = nvs_set_str(handle, "state", s_info.state);
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK)
        return err;

    s_update_mutex = xSemaphoreCreateMutex();
    if (!s_update_mutex)
        return ESP_ERR_NO_MEM;
    s_initialized = true;

    ESP_LOGW(FW_TAG, "versao=%s anterior=%s boot=%lu slot=%s OTA=%s estado=%s", s_info.current,
             s_info.previous[0] ? s_info.previous : "nenhuma", (unsigned long)s_info.boot_count,
             s_info.running_partition, s_info.ota_capable ? "sim" : "nao", s_info.state);
    return ESP_OK;
}

const firmware_ota_info_t *firmware_ota_get_info(void) { return &s_info; }
const char *firmware_ota_current_version(void) { return s_info.current[0] ? s_info.current : "unknown"; }

static void ble_ota_release(bool failed) {
    if (s_ble_ota.handle) {
        (void)esp_ota_abort(s_ble_ota.handle);
    }
    memset(&s_ble_ota, 0, sizeof(s_ble_ota));
    if (failed) {
        (void)set_state("failed");
    }
    if (s_update_mutex) {
        xSemaphoreGive(s_update_mutex);
    }
}

bool firmware_ota_ble_is_active(void) { return s_ble_ota.active; }

esp_err_t firmware_ota_ble_begin(uint32_t image_size) {
    if (!s_initialized || !s_info.ota_capable || image_size == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ble_ota.active || xSemaphoreTake(s_update_mutex, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition || image_size > partition->size) {
        xSemaphoreGive(s_update_mutex);
        return ESP_ERR_INVALID_SIZE;
    }

    memset(&s_ble_ota, 0, sizeof(s_ble_ota));
    s_ble_ota.partition = partition;
    s_ble_ota.expected_size = image_size;
    s_ble_ota.last_log_percent = -10;
    (void)set_state("download");

    esp_err_t err = esp_ota_begin(partition, image_size, &s_ble_ota.handle);
    if (err != ESP_OK) {
        ESP_LOGE(FW_TAG, "Falha ao iniciar OTA BLE: %s", esp_err_to_name(err));
        ble_ota_release(true);
        return err;
    }

    s_ble_ota.active = true;
    ESP_LOGW(FW_TAG, "OTA BLE iniciada: %lu bytes -> %s", (unsigned long)image_size, partition->label);
    return ESP_OK;
}

esp_err_t firmware_ota_ble_write(uint32_t offset, const uint8_t *data, size_t length, uint8_t *progress_percent) {
    if (!s_ble_ota.active || !data || length == 0 || offset != s_ble_ota.received_size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (length > s_ble_ota.expected_size - s_ble_ota.received_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_ota_write(s_ble_ota.handle, data, length);
    if (err != ESP_OK) {
        ESP_LOGE(FW_TAG, "Falha ao gravar OTA BLE no offset %lu: %s", (unsigned long)offset,
                 esp_err_to_name(err));
        ble_ota_release(true);
        return err;
    }

    s_ble_ota.received_size += (uint32_t)length;
    int percent = (int)(((uint64_t)s_ble_ota.received_size * 100U) / s_ble_ota.expected_size);
    if (percent > 100) percent = 100;
    if (progress_percent) *progress_percent = (uint8_t)percent;
    int milestone = (percent / 10) * 10;
    if (milestone >= s_ble_ota.last_log_percent + 10) {
        s_ble_ota.last_log_percent = milestone;
        ESP_LOGI(FW_TAG, "OTA BLE: %d%% (%lu/%lu bytes)", percent, (unsigned long)s_ble_ota.received_size,
                 (unsigned long)s_ble_ota.expected_size);
    }
    return ESP_OK;
}

esp_err_t firmware_ota_ble_finish(char *received_version, size_t received_version_size) {
    if (!s_ble_ota.active) return ESP_ERR_INVALID_STATE;
    if (s_ble_ota.received_size != s_ble_ota.expected_size) {
        ESP_LOGE(FW_TAG, "OTA BLE incompleta: %lu/%lu bytes", (unsigned long)s_ble_ota.received_size,
                 (unsigned long)s_ble_ota.expected_size);
        ble_ota_release(true);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_ota_handle_t handle = s_ble_ota.handle;
    s_ble_ota.handle = 0;
    esp_err_t err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(FW_TAG, "Binario OTA BLE invalido: %s", esp_err_to_name(err));
        ble_ota_release(true);
        return err;
    }

    esp_app_desc_t incoming = {0};
    const size_t descriptor_offset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    err = esp_partition_read(s_ble_ota.partition, descriptor_offset, &incoming, sizeof(incoming));
    const esp_app_desc_t *running = esp_app_get_description();
    if (err != ESP_OK || incoming.magic_word != ESP_APP_DESC_MAGIC_WORD || !running ||
        strncmp(incoming.project_name, running->project_name, sizeof(incoming.project_name)) != 0) {
        ESP_LOGE(FW_TAG, "Binario OTA BLE pertence a outro projeto ou esta corrompido");
        ble_ota_release(true);
        return err == ESP_OK ? ESP_ERR_INVALID_ARG : err;
    }

    err = esp_ota_set_boot_partition(s_ble_ota.partition);
    if (err != ESP_OK) {
        ESP_LOGE(FW_TAG, "Falha ao selecionar particao OTA BLE: %s", esp_err_to_name(err));
        ble_ota_release(true);
        return err;
    }

    if (received_version && received_version_size > 0) strlcpy(received_version, incoming.version, received_version_size);
    strlcpy(s_info.target, incoming.version, sizeof(s_info.target));
    (void)nvs_save_text("target", s_info.target);
    (void)set_state("ready");
    ESP_LOGW(FW_TAG, "OTA BLE pronta: %s -> %s", firmware_ota_current_version(), incoming.version);
    ble_ota_release(false);
    return ESP_OK;
}

void firmware_ota_ble_abort(void) {
    if (!s_ble_ota.active) return;
    ESP_LOGW(FW_TAG, "OTA BLE cancelada em %lu/%lu bytes", (unsigned long)s_ble_ota.received_size,
             (unsigned long)s_ble_ota.expected_size);
    ble_ota_release(true);
}

esp_err_t firmware_ota_mark_running_valid(void) {
    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK)
            return err;
        ESP_LOGW(FW_TAG, "Imagem OTA validada apos 30 segundos de saude");
    }
    s_info.target[0] = '\0';
    (void)nvs_save_text("target", "");
    return set_state(s_info.ota_capable ? "valid" : "no_ota");
}

static void validation_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS((uint32_t)(uintptr_t)arg));
    esp_err_t err = firmware_ota_mark_running_valid();
    if (err != ESP_OK)
        ESP_LOGE(FW_TAG, "Falha ao validar imagem: %s", esp_err_to_name(err));
    s_validation_scheduled = false;
    vTaskDelete(NULL);
}

esp_err_t firmware_ota_schedule_validation(uint32_t delay_ms) {
    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;
    if (s_validation_scheduled)
        return ESP_OK;
    if (delay_ms < 1000)
        delay_ms = 1000;
    if (xTaskCreate(validation_task, "fw_validate", 3072, (void *)(uintptr_t)delay_ms, 3, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    s_validation_scheduled = true;
    return ESP_OK;
}

static bool parse_version(const char *text, uint32_t parts[3]) {
    if (!text || !text[0])
        return false;

    memset(parts, 0, sizeof(uint32_t) * 3);
    const char *cursor = text;
    for (size_t index = 0; index < 3; index++) {
        if (*cursor < '0' || *cursor > '9')
            return false;

        uint32_t value = 0;
        while (*cursor >= '0' && *cursor <= '9') {
            if (value > 1000000U)
                return false;
            value = (value * 10U) + (uint32_t)(*cursor - '0');
            cursor++;
        }
        parts[index] = value;

        if (*cursor == '\0')
            return true;
        if (*cursor != '.')
            return false;
        cursor++;
    }

    return *cursor == '\0';
}

static int compare_versions(const char *left, const char *right) {
    uint32_t left_parts[3];
    uint32_t right_parts[3];
    if (!parse_version(left, left_parts) || !parse_version(right, right_parts))
        return 0;

    for (size_t index = 0; index < 3; index++) {
        if (left_parts[index] > right_parts[index])
            return 1;
        if (left_parts[index] < right_parts[index])
            return -1;
    }
    return 0;
}

static bool versions_equal(const char *left, const char *right) {
    uint32_t left_parts[3];
    uint32_t right_parts[3];
    if (parse_version(left, left_parts) && parse_version(right, right_parts)) {
        for (size_t index = 0; index < 3; index++) {
            if (left_parts[index] != right_parts[index])
                return false;
        }
        return true;
    }
    return left && right && strcmp(left, right) == 0;
}

static esp_err_t ota_metadata_event(esp_http_client_event_t *event) {
    ota_metadata_response_t *response = (ota_metadata_response_t *)event->user_data;
    if (!response || event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0)
        return ESP_OK;

    size_t available = sizeof(response->body) - 1U - response->length;
    if ((size_t)event->data_len > available) {
        response->overflow = true;
        return ESP_FAIL;
    }

    memcpy(response->body + response->length, event->data, (size_t)event->data_len);
    response->length += (size_t)event->data_len;
    response->body[response->length] = '\0';
    return ESP_OK;
}

static esp_err_t fetch_remote_version(char *version, size_t version_size) {
    ota_metadata_response_t response = {0};
    ESP_LOGI(FW_TAG, "Consultando metadados OTA do arquivo %d", OTA_FILE_ID);

    esp_http_client_config_t http = {
        .url = OTA_METADATA_URL,
        .cert_pem = rootCaCerticate,
        .event_handler = ota_metadata_event,
        .user_data = &response,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
#if GTW_USE_IPV6
        .addr_type = HTTP_ADDR_TYPE_INET6,
#else
        .addr_type = HTTP_ADDR_TYPE_INET,
#endif
    };

    esp_http_client_handle_t client = esp_http_client_init(&http);
    if (!client)
        return ESP_ERR_NO_MEM;

    esp_err_t err = esp_http_client_set_header(client, "x-ota-key", OTA_ACCESS_KEY);
    if (err == ESP_OK)
        err = esp_http_client_set_header(client, "Accept", "application/json");
    if (err == ESP_OK)
        err = esp_http_client_perform(client);

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(FW_TAG, "Falha ao consultar OTA %d: %s", OTA_FILE_ID, esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGE(FW_TAG, "Consulta OTA %d retornou HTTP %d", OTA_FILE_ID, status);
        return ESP_FAIL;
    }
    if (response.overflow || response.length == 0) {
        ESP_LOGE(FW_TAG, "Resposta de versao OTA vazia ou muito grande");
        return ESP_ERR_INVALID_SIZE;
    }

    cJSON *root = cJSON_ParseWithLength(response.body, response.length);
    if (!root)
        return ESP_FAIL;

    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *remote_version = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (!cJSON_IsNumber(id) || id->valueint != OTA_FILE_ID ||
        (!cJSON_IsNumber(remote_version) && !cJSON_IsString(remote_version))) {
        cJSON_Delete(root);
        ESP_LOGE(FW_TAG, "Resposta OTA com id ou versao invalida");
        return ESP_FAIL;
    }

    int written = 0;
    if (cJSON_IsString(remote_version) && remote_version->valuestring)
        written = snprintf(version, version_size, "%s", remote_version->valuestring);
    else
        written = snprintf(version, version_size, "%.9g", remote_version->valuedouble);
    cJSON_Delete(root);

    if (written <= 0 || written >= (int)version_size)
        return ESP_ERR_INVALID_SIZE;

    ESP_LOGI(FW_TAG, "Metadados OTA recebidos: arquivo=%d versao=%s", OTA_FILE_ID, version);
    return ESP_OK;
}

static esp_err_t ota_http_init(esp_http_client_handle_t client) {
    esp_err_t err = ESP_OK;
    if (s_auth_header[0])
        err = esp_http_client_set_header(client, "Authorization", s_auth_header);
    if (err == ESP_OK && s_ota_access_key[0])
        err = esp_http_client_set_header(client, "x-ota-key", s_ota_access_key);
    return err;
}

esp_err_t firmware_ota_update_https(const char *url, const char *bearer_token, const char *ota_access_key,
                                    const char *expected_version) {
    if (!s_initialized || !s_info.ota_capable)
        return ESP_ERR_INVALID_STATE;
    if (!url || strncmp(url, "https://", 8) != 0)
        return ESP_ERR_INVALID_ARG;
    if (expected_version && strlen(expected_version) >= FW_VERSION_TEXT_MAX)
        return ESP_ERR_INVALID_SIZE;
    if (xSemaphoreTake(s_update_mutex, 0) != pdTRUE)
        return ESP_ERR_INVALID_STATE;

    s_auth_header[0] = '\0';
    s_ota_access_key[0] = '\0';
    if (bearer_token && bearer_token[0]) {
        int n = snprintf(s_auth_header, sizeof(s_auth_header), "Bearer %s", bearer_token);
        if (n < 0 || n >= (int)sizeof(s_auth_header)) {
            xSemaphoreGive(s_update_mutex);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    if (ota_access_key && ota_access_key[0]) {
        size_t key_length = strlen(ota_access_key);
        if (key_length >= sizeof(s_ota_access_key)) {
            xSemaphoreGive(s_update_mutex);
            return ESP_ERR_INVALID_SIZE;
        }
        strlcpy(s_ota_access_key, ota_access_key, sizeof(s_ota_access_key));
    }
    if (expected_version && expected_version[0]) {
        strlcpy(s_info.target, expected_version, sizeof(s_info.target));
        (void)nvs_save_text("target", s_info.target);
    }
    (void)set_state("download");

    esp_http_client_config_t http = {
        .url = url,
        .cert_pem = rootCaCerticate,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
#if GTW_USE_IPV6
        .addr_type = HTTP_ADDR_TYPE_INET6,
#else
        .addr_type = HTTP_ADDR_TYPE_INET,
#endif
    };
    esp_https_ota_config_t ota = {.http_config = &http, .http_client_init_cb = ota_http_init};
    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota, &handle);
    if (err != ESP_OK)
        goto done;

    esp_app_desc_t incoming = {0};
    err = esp_https_ota_get_img_desc(handle, &incoming);
    if (err != ESP_OK)
        goto abort;
    const esp_app_desc_t *running = esp_app_get_description();
    if (!running || strcmp(incoming.project_name, running->project_name) != 0) {
        ESP_LOGE(FW_TAG, "Binario de outro projeto rejeitado: %s", incoming.project_name);
        err = ESP_ERR_INVALID_ARG;
        goto abort;
    }
    if (expected_version && expected_version[0] && !versions_equal(incoming.version, expected_version)) {
        ESP_LOGE(FW_TAG, "Versao recebida=%s esperada=%s", incoming.version, expected_version);
        err = ESP_ERR_INVALID_VERSION;
        goto abort;
    }
    if (versions_equal(incoming.version, s_info.current)) {
        err = ESP_ERR_INVALID_STATE;
        goto abort;
    }

    ESP_LOGW(FW_TAG, "Baixando %s -> %s", s_info.current, incoming.version);
    do {
        err = esp_https_ota_perform(handle);
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        if (err == ESP_OK)
            err = ESP_ERR_INVALID_SIZE;
        goto abort;
    }

    err = esp_https_ota_finish(handle);
    handle = NULL;
    if (err == ESP_OK) {
        strlcpy(s_info.target, incoming.version, sizeof(s_info.target));
        (void)nvs_save_text("target", s_info.target);
        (void)set_state("ready");
        ESP_LOGW(FW_TAG, "OTA pronta; reinicie para testar %s", incoming.version);
    }
    goto done;

abort:
    if (handle)
        (void)esp_https_ota_abort(handle);
done:
    if (err != ESP_OK) {
        ESP_LOGE(FW_TAG, "OTA falhou: %s", esp_err_to_name(err));
        (void)set_state("failed");
    }
    s_auth_header[0] = '\0';
    s_ota_access_key[0] = '\0';
    xSemaphoreGive(s_update_mutex);
    return err;
}

static esp_err_t check_for_update_once(bool *updated) {
    *updated = false;
    char remote_version[FW_VERSION_TEXT_MAX] = {0};
    esp_err_t err = fetch_remote_version(remote_version, sizeof(remote_version));
    if (err != ESP_OK)
        return err;

    uint32_t current_parts[3];
    uint32_t remote_parts[3];
    if (!parse_version(firmware_ota_current_version(), current_parts) || !parse_version(remote_version, remote_parts)) {
        ESP_LOGE(FW_TAG, "Versao local ou remota invalida: local=%s remota=%s", firmware_ota_current_version(),
                 remote_version);
        return ESP_ERR_INVALID_VERSION;
    }

    int comparison = compare_versions(remote_version, firmware_ota_current_version());
    ESP_LOGI(FW_TAG, "Versao local=%s remota=%s arquivo=%d", firmware_ota_current_version(), remote_version,
             OTA_FILE_ID);
    if (comparison <= 0) {
        ESP_LOGI(FW_TAG, "Firmware ja esta atualizado; nenhuma imagem sera baixada");
        return ESP_OK;
    }

    err = firmware_ota_update_https(OTA_DOWNLOAD_URL, NULL, OTA_ACCESS_KEY, remote_version);
    if (err == ESP_OK)
        *updated = true;
    return err;
}

static void ota_check_task(void *arg) {
    (void)arg;
    ESP_LOGI(FW_TAG, "Iniciando verificacao OTA do arquivo %d", OTA_FILE_ID);

    for (unsigned attempt = 1; attempt <= OTA_CHECK_MAX_ATTEMPTS; attempt++) {
        bool updated = false;
        esp_err_t err = ESP_ERR_TIMEOUT;

        ESP_LOGI(FW_TAG, "Tentativa %u/%u: aguardando acesso HTTP", attempt, OTA_CHECK_MAX_ATTEMPTS);
        if (MutexHTTP && xSemaphoreTake(MutexHTTP, pdMS_TO_TICKS(OTA_HTTP_TIMEOUT_MS)) == pdTRUE) {
            ESP_LOGI(FW_TAG, "Acesso HTTP liberado; comparando versoes");
            err = check_for_update_once(&updated);
            xSemaphoreGive(MutexHTTP);
        } else {
            ESP_LOGW(FW_TAG, "HTTP ocupado; verificacao OTA adiada");
        }

        if (err == ESP_OK) {
            if (updated) {
                ESP_LOGW(FW_TAG, "Atualizacao concluida; reiniciando em 2 segundos");
                vTaskDelay(pdMS_TO_TICKS(2000));
                esp_restart();
            }
            break;
        }

        ESP_LOGW(FW_TAG, "Tentativa OTA %u/%u falhou: %s", attempt, OTA_CHECK_MAX_ATTEMPTS,
                 esp_err_to_name(err));
        if (attempt < OTA_CHECK_MAX_ATTEMPTS)
            vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_RETRY_DELAY_MS));
    }

    s_check_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t firmware_ota_check_for_update_async(void) {
    if (!s_initialized || !s_info.ota_capable)
        return ESP_ERR_INVALID_STATE;
    if (s_check_task)
        return ESP_OK;
    if (strlen(OTA_ACCESS_KEY) < 32U)
        return ESP_ERR_INVALID_SIZE;

    if (xTaskCreate(ota_check_task, "ota_check", 10240, NULL, 4, &s_check_task) != pdPASS) {
        s_check_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
