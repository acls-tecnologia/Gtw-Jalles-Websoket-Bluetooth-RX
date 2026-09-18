#include "config.h" //Configuração do projeto
#include "firmware_ota.h"
#include "network_mode.h"

#define BLE_STARTUP_WINDOW_MS (30 * 1000U)
#define HTTP_MUTEX_WAIT_MS 20000U
#if (GTW_ROLE_TX_ONLY == 0)
#define PATCH_BACKOFF_MIN_MS 2000U
#define PATCH_BACKOFF_MAX_MS 30000U
#define PATCH_BACKOFF_JITTER_MS 1000U
#define PATCH_MAX_ATTEMPTS 5U
#endif
#define GTW_RX_DUP_CACHE_SIZE 64
#define GTW_TANK_SEEN_CACHE_SIZE 64

#if (GTW_ROLE_TX_ONLY == 0)

static PatchRequest patchPool[QUEUE_LENGTH]; // Array estático de objetos

typedef enum {
    PATCH_SLOT_FREE = 0,
    PATCH_SLOT_QUEUED,
    PATCH_SLOT_PROCESSING,
} patch_slot_state_t;

#endif

#if (GTW_ROLE_RX_ONLY == 0)
typedef struct {
    bool active;
    uint8_t msg_id;
    uint16_t tank_id;
    bool accept_any_frame;
    TaskHandle_t waiter;
} gtw_ack_wait_t;
#endif

#if (GTW_ROLE_TX_ONLY == 0)
typedef struct {
    bool used;
    uint16_t src_id;
    uint8_t msg_id;
    TickType_t accepted_at;
} gtw_rx_dup_t;
#endif

typedef struct {
    bool used;
    uint16_t tank_id;
    int64_t last_seen_ms;
} gtw_tank_seen_t;

typedef struct {
    uint16_t tank_id;
    const char *nome;
} gtw_tank_name_t;

typedef struct {
    uint32_t timeout;
    uint32_t frame_ok;
    uint32_t incomplete;
    uint32_t bad_preamble;
    uint32_t bad_crc;
    uint32_t semantic_invalid;
    uint32_t wrong_dst;
    uint32_t ack_rx;
    uint32_t ack_match;
    uint32_t ack_unexpected;
    uint32_t ping_rx;
    uint32_t data_rx;
    uint32_t duplicate;
    uint32_t queue_accept;
    uint32_t queue_reject;
    uint32_t ack_sent;
} gtw_lora_rx_stats_t;

#if (GTW_ROLE_RX_ONLY == 0)
static SemaphoreHandle_t gtw_ack_mutex = NULL;
#endif
static SemaphoreHandle_t gtw_request_mutex = NULL;
static volatile bool gtw_lora_ready = false;
#if (GTW_ROLE_TX_ONLY == 0)
static SemaphoreHandle_t patch_pool_mutex = NULL;
static patch_slot_state_t patchPoolState[QUEUE_LENGTH] = {0};
#endif
#if (GTW_ROLE_RX_ONLY == 0)
static gtw_ack_wait_t gtw_ack_wait = {0};
#endif
#if (GTW_ROLE_TX_ONLY == 0)
static gtw_rx_dup_t gtw_rx_dup_cache[GTW_RX_DUP_CACHE_SIZE] = {0};
#endif
static gtw_tank_seen_t gtw_tank_seen_cache[GTW_TANK_SEEN_CACHE_SIZE] = {0};
static const gtw_tank_name_t gtw_tank_names[] = {
    {1, "R-0 Nivel 1"},
    {21, "R-0 Nivel 2"},
    {22, "R-01"},
    {23, "R-02A"},
    {24, "R-04"},
    {29, "R-11"},
    {41, "Captacao Revolta"},
    {61, "RA-09"},
    {62, "RA-10"},
    {63, "RD-0"},
    {81, "RD-02"},
    {82, "RD-06"},
    {83, "RD-09"},
    {84, "Booster"},
    {25, "R-02"},
    {26, "R-03"},
    {27, "R-05"},
    {28, "ETAR"},
};
static volatile int64_t gtw_last_lora_rx_ms = 0;
static volatile bool ble_config_mode_active = false;
static volatile bool ble_startup_window_active = false;
static TaskHandle_t ble_config_mode_task_handle = NULL;

static void gtw_lora_log_rx_stats(const gtw_lora_rx_stats_t *s, const char *motivo) {
    if (!s)
        return;

    ESP_LOGW(
        "GTW_LORA_STATS",
        "motivo=%s timeout=%lu frame_ok=%lu incomplete=%lu preamble=%lu crc=%lu semantic=%lu wrong_dst=%lu "
        "ack_rx=%lu ack_match=%lu ack_unexp=%lu ping=%lu data=%lu dup=%lu queue_ok=%lu queue_fail=%lu ack_sent=%lu",
        motivo, (unsigned long)s->timeout, (unsigned long)s->frame_ok, (unsigned long)s->incomplete,
        (unsigned long)s->bad_preamble, (unsigned long)s->bad_crc, (unsigned long)s->semantic_invalid,
        (unsigned long)s->wrong_dst, (unsigned long)s->ack_rx, (unsigned long)s->ack_match,
        (unsigned long)s->ack_unexpected, (unsigned long)s->ping_rx, (unsigned long)s->data_rx,
        (unsigned long)s->duplicate, (unsigned long)s->queue_accept, (unsigned long)s->queue_reject,
        (unsigned long)s->ack_sent);
}

static void gtw_mark_tank_seen(uint16_t tank_id) {
    if (tank_id == 0)
        return;

    int slot = tank_id % GTW_TANK_SEEN_CACHE_SIZE;
    gtw_tank_seen_cache[slot].used = true;
    gtw_tank_seen_cache[slot].tank_id = tank_id;
    gtw_tank_seen_cache[slot].last_seen_ms = esp_timer_get_time() / 1000;
}

#if (GTW_ROLE_RX_ONLY == 0)
static bool gtw_tank_seen_recently(uint16_t tank_id, int64_t window_ms) {
    int slot = tank_id % GTW_TANK_SEEN_CACHE_SIZE;
    gtw_tank_seen_t *entry = &gtw_tank_seen_cache[slot];

    return entry->used && entry->tank_id == tank_id &&
           ((esp_timer_get_time() / 1000) - entry->last_seen_ms) <= window_ms;
}
#endif

static const char *gtw_tank_name_from_id(uint16_t tank_id, char *fallback, size_t fallback_len) {
    for (size_t i = 0; i < sizeof(gtw_tank_names) / sizeof(gtw_tank_names[0]); i++) {
        if (gtw_tank_names[i].tank_id == tank_id) {
            return gtw_tank_names[i].nome;
        }
    }

    snprintf(fallback, fallback_len, "tanque %u", tank_id);
    return fallback;
}

static bool gtw_expand_compact_alert_body(const char *body, uint16_t tank_id, char *out, size_t out_len) {
    if (!body || !out || out_len == 0)
        return false;

    cJSON *root = cJSON_Parse(body);
    if (!root)
        return false;

    cJSON *code_item = cJSON_GetObjectItemCaseSensitive(root, "a");
    if (!cJSON_IsNumber(code_item)) {
        cJSON_Delete(root);
        return false;
    }

    int code = code_item->valueint;
    int unidade = 1;
    int pct = -1;
    int bomba_id = -1;

    cJSON *unidade_item = cJSON_GetObjectItemCaseSensitive(root, "u");
    if (cJSON_IsNumber(unidade_item))
        unidade = unidade_item->valueint;

    cJSON *pct_item = cJSON_GetObjectItemCaseSensitive(root, "p");
    if (cJSON_IsNumber(pct_item))
        pct = pct_item->valueint;

    cJSON *bomba_item = cJSON_GetObjectItemCaseSensitive(root, "b");
    if (cJSON_IsNumber(bomba_item))
        bomba_id = bomba_item->valueint;

    char fallback[24];
    const char *tanque_nome = gtw_tank_name_from_id(tank_id, fallback, sizeof(fallback));
    char mensagem[180];

    switch (code) {
    case 1:
        snprintf(mensagem, sizeof(mensagem), "Botao de emergencia acionado no tanque %s", tanque_nome);
        break;
    case 2:
        if (bomba_id > 0)
            snprintf(mensagem, sizeof(mensagem), "ALERTA BOMBA PARADA: Tanque %s, Bomba %d, emergencia global acionada",
                     tanque_nome, bomba_id);
        else
            snprintf(mensagem, sizeof(mensagem), "ALERTA BOMBA PARADA: Tanque %s, emergencia global acionada",
                     tanque_nome);
        break;
    case 3:
        if (bomba_id > 0)
            snprintf(mensagem, sizeof(mensagem),
                     "ALERTA BOMBA: Tanque %s, Bomba %d, FALHA AO LIGAR BOMBA, emergencia global ativa", tanque_nome,
                     bomba_id);
        else
            snprintf(mensagem, sizeof(mensagem),
                     "ALERTA BOMBA: Tanque %s, FALHA AO LIGAR BOMBA, emergencia global ativa", tanque_nome);
        break;
    case 4:
        if (bomba_id > 0)
            snprintf(mensagem, sizeof(mensagem),
                     "ALERTA BOMBA: Tanque %s, Bomba %d, FALHA AO LIGAR BOMBA, retorno real nao confirmou em 30s",
                     tanque_nome, bomba_id);
        else
            snprintf(mensagem, sizeof(mensagem),
                     "ALERTA BOMBA: Tanque %s, FALHA AO LIGAR BOMBA, retorno real nao confirmou em 30s", tanque_nome);
        break;
    case 5:
        if (bomba_id > 0)
            snprintf(mensagem, sizeof(mensagem),
                     "ALERTA BOMBA: Tanque %s, Bomba %d, FALHA AO DESLIGAR BOMBA, retorno real nao confirmou em 30s",
                     tanque_nome, bomba_id);
        else
            snprintf(mensagem, sizeof(mensagem),
                     "ALERTA BOMBA: Tanque %s, FALHA AO DESLIGAR BOMBA, retorno real nao confirmou em 30s",
                     tanque_nome);
        break;
    case 6:
        if (pct >= 0)
            snprintf(mensagem, sizeof(mensagem), "ALERTA: NIVEL MINIMO ATINGIDO %s %d%%", tanque_nome, pct);
        else
            snprintf(mensagem, sizeof(mensagem), "ALERTA: NIVEL MINIMO ATINGIDO %s", tanque_nome);
        break;
    case 7:
        if (pct >= 0)
            snprintf(mensagem, sizeof(mensagem), "ALERTA: NIVEL MAXIMO ATINGIDO %s %d%%", tanque_nome, pct);
        else
            snprintf(mensagem, sizeof(mensagem), "ALERTA: NIVEL MAXIMO ATINGIDO %s", tanque_nome);
        break;
    case 8:
        if (pct >= 0)
            snprintf(mensagem, sizeof(mensagem), "ALERTA: NIVEL MINIMO ATINGIDO %s NIVEL 2 %d%%", tanque_nome, pct);
        else
            snprintf(mensagem, sizeof(mensagem), "ALERTA: NIVEL MINIMO ATINGIDO %s NIVEL 2", tanque_nome);
        break;
    case 9:
        if (pct >= 0)
            snprintf(mensagem, sizeof(mensagem), "ALERTA: NIVEL MAXIMO ATINGIDO %s NIVEL 2 %d%%", tanque_nome, pct);
        else
            snprintf(mensagem, sizeof(mensagem), "ALERTA: NIVEL MAXIMO ATINGIDO %s NIVEL 2", tanque_nome);
        break;
    default:
        snprintf(mensagem, sizeof(mensagem), "Alerta codigo %d recebido do tanque %s", code, tanque_nome);
        break;
    }

    int n = snprintf(out, out_len, "{\"mensagem\":\"%s\",\"unidade\":%d}", mensagem, unidade);
    cJSON_Delete(root);
    return n > 0 && n < (int)out_len;
}

static bool gtw_expand_short_lora_body(const char *body, uint8_t has_bomba_id, char *out, size_t out_len) {
    if (!body || !out || out_len == 0)
        return false;

    cJSON *root = cJSON_Parse(body);
    if (!root)
        return false;

    const char *api_field = NULL;
    cJSON *value = NULL;

    if (has_bomba_id == 0) {
        value = cJSON_GetObjectItemCaseSensitive(root, "n");
        if (cJSON_IsNumber(value))
            api_field = "nivel_atual";
    } else if (has_bomba_id == 1) {
        value = cJSON_GetObjectItemCaseSensitive(root, "s");
        if (cJSON_IsNumber(value)) {
            api_field = "status";
        } else {
            value = cJSON_GetObjectItemCaseSensitive(root, "m");
            if (cJSON_IsNumber(value)) {
                api_field = "automatico";
            } else {
                value = cJSON_GetObjectItemCaseSensitive(root, "v");
                if (cJSON_IsNumber(value))
                    api_field = "vazao";
            }
        }
    }

    bool ok = false;
    if (api_field && cJSON_IsNumber(value)) {
        int n;
        if (strcmp(api_field, "vazao") == 0)
            n = snprintf(out, out_len, "{\"%s\":%.2f}", api_field, value->valuedouble);
        else
            n = snprintf(out, out_len, "{\"%s\":%d}", api_field, value->valueint);
        ok = n > 0 && n < (int)out_len;
    }

    cJSON_Delete(root);
    return ok;
}

void wifi_task(void *pv);

static void tentar_conectar_wifi(void);

void login_task(void *pvParameters);

static bool restart_token_timer(void);

static void token_timer_cb(void *arg);

void ConnectRest();

#if (GTW_ROLE_RX_ONLY == 0)
void connect_to_websocket(void *pvParameters);

static void handle_bomba_control(cJSON *json);
#endif

void save_SIIDWifi(const char *device_name);

void save_PasswordWifi(const char *password);

void save_UsetGTW(const char *device_name);

void save_PasswordGTW(const char *password);

void save_idGtw(int state);

void save_idUnidadeGtw(int state);

#if (GTW_ROLE_RX_ONLY == 0)
static void ws_msg_processor_task(void *pv);

static void websocket_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
#endif

static void lora_setup_gtw(void);

void gtw_lora_rx_task(void *pvParameters);
static void gtw_lora_health_task(void *pvParameters);

uint16_t lora_crc16(const uint8_t *data, size_t len);
static size_t lora_frame_air_len(const lora_app_frame_t *frame);
static int lora_send_frame_air(lora_app_frame_t *frame);
static bool lora_received_frame_valid(const lora_app_frame_t *rx, int air_len, uint16_t *out_crc_calc,
                                      uint16_t *out_crc_rx);

#if (GTW_ROLE_TX_ONLY == 0)
bool queue_patch_request(const char *path, const char *body);
#endif

void vTaskImprimirUsoMemoria(void *pvParameters);

#if (GTW_ROLE_TX_ONLY == 0)
void gtw_send_ack(const lora_app_frame_t *rx);

void patch_task(void *pvParameters);
#endif

#if (GTW_ROLE_RX_ONLY == 0)
bool gtw_send_to_tank(uint16_t tank_id, const char *msg, uint16_t bomba_id);

static void handle_PingTanque(cJSON *json);

static void handle_BTOn(cJSON *json);

bool gtw_ping_tank(uint16_t tank_id);

bool ws_send_json(const char *json_msg);
#endif

void bt_message_received_callback(const char *message);
void bt_binary_received_callback(const uint8_t *data, size_t length);

void bt_client_connected_callback(void);
void bt_client_disconnected_callback(void);

static void bt_send_config_snapshot(void);
static void ble_ota_restart_task(void *pvParameters);
static void ble_config_mode_task(void *pvParameters);
static void ble_startup_window_task(void *pvParameters);

void app_main(void) {
    ESP_LOGI(TAG, "Inicializando sistema...");
    ESP_LOGI(TAG, "Modo de operacao: %s (TX_ONLY=%d RX_ONLY=%d)", GTW_ROLE_NAME, GTW_ROLE_TX_ONLY, GTW_ROLE_RX_ONLY);

    /****************************************
     * 1️⃣ Inicializa NVS
     ****************************************/
    esp_err_t errNVS = nvs_flash_init();

    if (errNVS == ESP_ERR_NVS_NO_FREE_PAGES || errNVS == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS corrompida ou cheia. Apagando...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        errNVS = nvs_flash_init();
    }

    if (errNVS != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao inicializar NVS: %s", esp_err_to_name(errNVS));
        ConnectRest();
    }

    ESP_ERROR_CHECK(firmware_ota_init());


    esp_err_t fw_validation_err = firmware_ota_schedule_validation(30000);
    if (fw_validation_err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao agendar validacao da imagem OTA: %s", esp_err_to_name(fw_validation_err));
    }

    /****************************************
     * 2️⃣ Criação de Filas
     ****************************************/

    // Pool de índices livres
#if (GTW_ROLE_TX_ONLY == 0)
    xPatchFreeQueue = xQueueCreate(QUEUE_LENGTH, sizeof(uint8_t));
    if (!xPatchFreeQueue) {
        ESP_LOGE(TAG, "Erro ao criar xPatchFreeQueue");
        ConnectRest();
    }

    // Preenche com índices (0..QUEUE_LENGTH-1)
    for (uint8_t i = 0; i < QUEUE_LENGTH; i++) {
        xQueueSend(xPatchFreeQueue, &i, 0);
    }

    // Fila principal de Patch Requests
    xPatchQueue = xQueueCreate(QUEUE_LENGTH, sizeof(PatchRequest *));
    if (!xPatchQueue) {
        ESP_LOGE(TAG, "Erro ao criar xPatchQueue");
        ConnectRest();
    }
#else
    ESP_LOGI(TAG, "Modo TX_ONLY: fila PATCH nao criada");
#endif

#if (GTW_ROLE_RX_ONLY == 0)
    // Fila mensagens WebSocket
    ws_msg_queue = xQueueCreate(WS_QUEUE_LEN, sizeof(ws_msg_item_t));
    if (!ws_msg_queue) {
        ESP_LOGE(TAG, "Erro ao criar ws_msg_queue");
        ConnectRest();
    }
#else
    ESP_LOGI(TAG, "Modo RX_ONLY: fila WebSocket nao criada");
#endif

    /****************************************
     * 3️⃣ Mutex
     ****************************************/
    MutexHTTP = xSemaphoreCreateMutex();
    if (!MutexHTTP) {
        ESP_LOGE(TAG, "Falha ao criar MutexHTTP");
        ConnectRest();
    }

    MutexLora = xSemaphoreCreateMutex();
    if (!MutexLora) {
        ESP_LOGE(TAG, "Falha ao criar MutexLora");
        ConnectRest();
    }

#if (GTW_ROLE_RX_ONLY == 0)
    gtw_ack_mutex = xSemaphoreCreateMutex();
#endif
    gtw_request_mutex = xSemaphoreCreateMutex();
#if (GTW_ROLE_TX_ONLY == 0)
    patch_pool_mutex = xSemaphoreCreateMutex();
#endif
    if (!gtw_request_mutex
#if (GTW_ROLE_RX_ONLY == 0)
        || !gtw_ack_mutex
#endif
#if (GTW_ROLE_TX_ONLY == 0)
        || !patch_pool_mutex
#endif
    ) {
        ESP_LOGE(TAG, "Falha ao criar controle de ACK LoRa");
        ConnectRest();
    }

    /****************************************
     * 4️⃣ Credenciais: NVS primeiro, valores fixos como fallback
     ****************************************/

    load_save_SIIDWifi(wifi_ssid, sizeof(wifi_ssid));
    load_PasswordWifi(wifi_password, sizeof(wifi_password));
    load_save_UsetGTW(userNameHTTPs, sizeof(userNameHTTPs));
    load_PasswordGTW(passwordHTTPs, sizeof(passwordHTTPs));
    ID_GATEWAY = load_idGTW();
    DEVICE_ID = load_idUnidadeGTW();

    // if (wifi_ssid[0] == '\0') {
    //     strncpy(wifi_ssid, "Acls_R", sizeof(wifi_ssid));
    // }
    // if (wifi_password[0] == '\0') {
    //     strncpy(wifi_password, "Acls@1234", sizeof(wifi_password));
    // }
    // if (userNameHTTPs[0] == '\0') {
    //     strncpy(userNameHTTPs, "ACLSGTWTEST", sizeof(userNameHTTPs));
    // }
    // if (passwordHTTPs[0] == '\0') {
    //     strncpy(passwordHTTPs, "Acls@123", sizeof(passwordHTTPs));
    // }
    // if (ID_GATEWAY == 0) {
    //     ID_GATEWAY = 3;
    // }
    // if (DEVICE_ID == 0) {
    //     DEVICE_ID = ID_GATEWAY;
    // }

    if (wifi_ssid[0] == '\0' || wifi_password[0] == '\0' || ID_GATEWAY == 0 || DEVICE_ID == 0 ||
        userNameHTTPs[0] == '\0' || passwordHTTPs[0] == '\0') {

#if DEBUG_MODE
        ESP_LOGE("Main", "Algumas configurações não estão configuradas! Aguardando configuração via Bluetooth.");
#endif
        int initBT = 0;

        while (wifi_ssid[0] == '\0' || wifi_password[0] == '\0' || ID_GATEWAY == 0 || DEVICE_ID == 0 ||
               userNameHTTPs[0] == '\0' || passwordHTTPs[0] == '\0') {

            if (initBT == 0) {
                bluetooth_config_start(BLUETOOTH_CONFIG_TIMEOUT_FOREVER_MS);
                printf("Bluetooth iniciado para configuração.\n");
            }

            if (wifi_ssid[0] == '\0') {
                ESP_LOGE("NVS", "Wifi_ssid não configurado.");
            }
            if (wifi_password[0] == '\0') {
                ESP_LOGE("NVS", "Wifi_password não configurado.");
            }

            if (ID_GATEWAY == 0) {
                ESP_LOGE("NVS", "ID_GATEWAY não configurado.");
            }
            if (DEVICE_ID == 0) {
                ESP_LOGE("NVS", "ID_UNIDADE não configurado.");
            }
            if (userNameHTTPs[0] == '\0') {
                ESP_LOGE("NVS", "userNameHTTPs não configurado.");
            }
            if (passwordHTTPs[0] == '\0') {
                ESP_LOGE("NVS", "passwordHTTPs não configurado.");
            }

            if (initBT == 0) {
                initBT++;
            }

            vTaskDelay(pdMS_TO_TICKS(5000)); // Aguardar 5 segundo antes de verificar novamente
        }
    }

    /****************************************
     * 5️⃣ Configuração LoRa
     ****************************************/
    ESP_LOGI(TAG, "Configurando LoRa...");
    lora_setup_gtw();

    /****************************************
     * 6️⃣ Tasks
     ****************************************/
    ESP_LOGI(TAG, "Criando tasks...");

    ESP_LOGI(TAG, "BLE aberto no boot por 30 segundos; Wi-Fi inicia somente depois da janela BLE");
    ble_startup_window_active = true;
    esp_err_t ble_start_err = bluetooth_config_start(BLE_STARTUP_WINDOW_MS);
    if (ble_start_err != ESP_OK) {
        ble_startup_window_active = false;
        ESP_LOGE(TAG, "Falha ao abrir BLE no boot: %s", esp_err_to_name(ble_start_err));
    } else if (xTaskCreate(ble_startup_window_task, "ble_boot_wait", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar task da janela BLE; fechando BLE para liberar o Wi-Fi");
        bluetooth_config_stop();
        ble_startup_window_active = false;
    }

    if (xTaskCreate(wifi_task, "Conecta_Wifi", 4096, NULL, 5, &taskConecta_WIFI) != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar task WiFi");
        ConnectRest();
    }

    if (xTaskCreate(gtw_lora_rx_task, "gtw_lora_rx_task", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar task LoRa RX");
        ConnectRest();
    }

    if (xTaskCreate(gtw_lora_health_task, "gtw_lora_health", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar task de saúde LoRa");
        ConnectRest();
    }

#if (GTW_ROLE_RX_ONLY == 0)
    if (xTaskCreate(ws_msg_processor_task, "ws_msg_proc", 9216, NULL, 4, NULL) != pdPASS)
        ESP_LOGE(TAG, "Falha ao criar task WebSocket Processor");
#else
    ESP_LOGW(TAG, "Modo RX_ONLY: WebSocket Processor desativado");
#endif

    if (xTaskCreate(vTaskImprimirUsoMemoria, "MonitorMemoria", 6144, NULL, 3, &taskImprimirUsoMemoria) != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar task Monitor de Memória");
        ConnectRest();
    }

    /****************************************
     * 7️⃣ Sistema inicializado
     ****************************************/
    ESP_LOGI(TAG, "Sistema inicializado com sucesso.");
}

static void tentar_conectar_wifi(void) {
    if (ble_config_mode_active || ble_startup_window_active) {
        ESP_LOGW(TAG, "Janela BLE ativa; Wi-Fi nao sera iniciado");
        return;
    }

    if (!wifi_start_driver()) {
        ESP_LOGE(TAG, "Falha ao iniciar driver Wi-Fi");
        return;
    }

    if (usando_secundario && wifi_secundario) {
        ESP_LOGI(TAG, "Conectando ao Wi-Fi SECUNDÁRIO (%s)", WIFI_SECONDARY_SSID);
        wifi_connect_credentials(WIFI_SECONDARY_SSID, WIFI_SECONDARY_PASS, WIFI_CONNECT_TIMEOUT_MS);
    } else {
        ESP_LOGI(TAG, "Conectando ao Wi-Fi PRIMÁRIO (%s)", wifi_ssid);
        wifi_connect_credentials(wifi_ssid, wifi_password, WIFI_CONNECT_TIMEOUT_MS);
    }
}

void wifi_task(void *pv) {
    ESP_LOGI(TAG, "Iniciando rotina de Wi-Fi...");
    esp_task_wdt_add(NULL);
    tentar_conectar_wifi();

    static int falhas_wifi = 0;
    static int falhas_ip = 0;
    static int falhas_internet = 0;
    static int ciclos_religar = 0;
    int64_t inicio_falha_internet_ms = 0;
    int wifi_4G = 0;
    bool ota_checked_for_connection = false;

    while (1) {
        esp_task_wdt_reset();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WIFI_MONITOR_INTERVAL_MS));

        if (ble_config_mode_active || ble_startup_window_active) {
            if (wifi_is_active()) {
                ESP_LOGW(TAG, "Janela BLE ativa; desligando Wi-Fi");
                wifi_stop_driver();
            }
            Connectado = 0;
            ota_checked_for_connection = false;
            continue;
        }

        if (!wifi_is_active()) {
            ota_checked_for_connection = false;
            ESP_LOGW(TAG, "Driver Wi-Fi inativo — reiniciando driver...");
            tentar_conectar_wifi();

            if (!wifi_is_active()) {
                ciclos_religar++;
                ESP_LOGE(TAG, "Driver Wi-Fi nao iniciou (%d/%d)", ciclos_religar, WIFI_CYCLE_MAX_RETRIES);
                if (ciclos_religar >= WIFI_CYCLE_MAX_RETRIES) {
                    ESP_LOGE(TAG, "Driver Wi-Fi permaneceu inativo; reiniciando ESP32");
                    esp_restart();
                }
            } else {
                falhas_wifi = 0;
            }
            continue;
        }

        // === Está conectado ao AP? ===
        if (wifi_sta_connected()) {
            falhas_wifi = 0;

            if (!wifi_sta_has_ip()) {
                Connectado = 0;
                ota_checked_for_connection = false;
                falhas_internet = 0;
                inicio_falha_internet_ms = 0;
                falhas_ip++;
                ESP_LOGW(TAG, "Associado ao roteador, mas sem IP (%d/%d)", falhas_ip, WIFI_IP_MAX_FAILS);

                if (falhas_ip >= WIFI_IP_MAX_FAILS) {
                    ESP_LOGW(TAG, "Sem IP por 60 segundos; reiniciando driver Wi-Fi");
                    wifi_stop_driver();
                    vTaskDelay(pdMS_TO_TICKS(WIFI_DRIVER_RESTART_DELAY_MS));

                    if (wifi_secundario)
                        usando_secundario = !usando_secundario;

                    tentar_conectar_wifi();
                    falhas_ip = 0;
                    ciclos_religar++;
                    if (ciclos_religar >= WIFI_CYCLE_MAX_RETRIES) {
                        ESP_LOGE(TAG, "Sem IP apos %d ciclos; reiniciando ESP32", ciclos_religar);
                        esp_restart();
                    }
                }
                continue;
            }

            falhas_ip = 0;
            ciclos_religar = 0;

            // O login/API deve continuar tentando sempre que houver Wi-Fi e IP valido,
            // mesmo se o diagnostico externo do Google estiver temporariamente indisponivel.
            if (Task_login_task == NULL) {
                if (xTaskCreate(login_task, "login_task", 1024 * 8, NULL, 5, &Task_login_task) != pdPASS) {
                    Task_login_task = NULL;
                    ESP_LOGE(TAG, "Falha ao criar login_task; nova tentativa no proximo ciclo");
                } else {
                    ESP_LOGI(TAG, "Task de login criada; aguardando resposta da API");
                }
            }

            if (wifi_check_internet(INTERNET_CHECK_TIMEOUT_MS)) {
                falhas_internet = 0;
                inicio_falha_internet_ms = 0;
#if DEBUG_MODE
                ESP_LOGI(TAG, "Conectado e com internet!");
#endif

                Connectado = 1;

                if (!ota_checked_for_connection) {
                    esp_err_t ota_err = firmware_ota_check_for_update_async();
                    if (ota_err == ESP_OK) {
                        ota_checked_for_connection = true;
                        ESP_LOGI(TAG, "Verificacao OTA do arquivo %d agendada", OTA_FILE_ID);
                    } else {
                        ESP_LOGW(TAG, "Nao foi possivel agendar verificacao OTA: %s", esp_err_to_name(ota_err));
                    }
                }

                if (wifi_secundario) {
                    if (TokenOk == 1 && wifi_secundario_ativo == 1 && wifi_4G == 0) {
                        wifi_4G = 1;
                    }
                }
            } else {
                Connectado = 0;
                ota_checked_for_connection = false;
                falhas_internet++;
                int64_t agora_ms = esp_timer_get_time() / 1000;
                if (inicio_falha_internet_ms == 0)
                    inicio_falha_internet_ms = agora_ms;

                uint32_t sem_internet_ms = (uint32_t)(agora_ms - inicio_falha_internet_ms);
                ESP_LOGW(TAG, "Wi-Fi associado, mas sem internet ha %lu s (falha %d)",
                         (unsigned long)(sem_internet_ms / 1000U), falhas_internet);

                if (sem_internet_ms >= WIFI_INTERNET_RESTART_DELAY_MS) {
                    ESP_LOGW(TAG, "Sem internet por 5 minutos; reiniciando somente o driver Wi-Fi");
                    falhas_internet = 0;
                    inicio_falha_internet_ms = 0;

                    wifi_stop_driver();
                    vTaskDelay(pdMS_TO_TICKS(WIFI_DRIVER_RESTART_DELAY_MS));

                    if (wifi_secundario) {
                        usando_secundario = !usando_secundario;
                        if (!usando_secundario) {
                            wifi_4G = 0;
                        }
                    }

                    tentar_conectar_wifi();
                }
            }
        } else {
            // Não conectado ao AP
            Connectado = 0;
            ota_checked_for_connection = false;
            falhas_ip = 0;
            falhas_internet = 0;
            inicio_falha_internet_ms = 0;
            falhas_wifi++;
            ESP_LOGW(TAG, "Wi-Fi desconectado (%d/%d)", falhas_wifi, WIFI_CONNECT_MAX_FAILS);

            if (falhas_wifi < WIFI_CONNECT_MAX_FAILS) {
                tentar_conectar_wifi();
                continue;
            }

            if (falhas_wifi >= WIFI_CONNECT_MAX_FAILS) {
                ESP_LOGE(TAG, "Falhou reconexão Wi-Fi — reiniciando ciclo");

                wifi_stop_driver();

                vTaskDelay(pdMS_TO_TICKS(WIFI_DRIVER_RESTART_DELAY_MS));

                if (wifi_secundario)
                    usando_secundario = !usando_secundario;

                tentar_conectar_wifi();

                falhas_wifi = 0;
                ciclos_religar++;
            }
        }

        if (ciclos_religar >= WIFI_CYCLE_MAX_RETRIES) {
            ESP_LOGE(TAG, "❌ Falhou após %d ciclos — Reiniciando ESP32", ciclos_religar);
            esp_restart();
        }
    }
}

// Callback do timer: notifica a task
static void token_timer_cb(void *arg) {
    if (Task_login_task != NULL)
        xTaskNotify(Task_login_task, 1, eSetValueWithOverwrite);
}

static bool restart_token_timer(void) {
    if (token_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = &token_timer_cb, .arg = NULL, .dispatch_method = ESP_TIMER_TASK, .name = "token_timer"};
        esp_err_t create_err = esp_timer_create(&timer_args, &token_timer);
        if (create_err != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao criar timer do token: %s", esp_err_to_name(create_err));
            token_timer = NULL;
            return false;
        }
    }

    if (esp_timer_is_active(token_timer)) {
        esp_err_t stop_err = esp_timer_stop(token_timer);
        if (stop_err != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao parar timer anterior do token: %s", esp_err_to_name(stop_err));
            return false;
        }
    }

    esp_err_t start_err = esp_timer_start_once(token_timer, 20ULL * 3600ULL * 1000000ULL);
    if (start_err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar timer de renovacao do token: %s", esp_err_to_name(start_err));
        return false;
    }

    ESP_LOGI(TAG, "Renovacao do token agendada para daqui a 20 horas");
    return true;
}

void login_task(void *pvParameters) {

    Task_login_task = xTaskGetCurrentTaskHandle(); // salva o handle

    int taskInit = 0;
    while (1) {
#if DEBUG_MODE
        UBaseType_t stack_remain = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGI("STACK", "\033[1;35m*** Task [%s] - mínimo livre: %u words (~%u bytes) ***\033[0m",
                 pcTaskGetName(NULL), stack_remain, stack_remain * sizeof(StackType_t));
#endif

        TokenOk = token_global[0] != '\0';

        if (wifi_sta_connected() && wifi_sta_has_ip()) {
            if (!verifica_conexao_internet()) {
                ESP_LOGW(TAG, "Diagnostico Google/DNS falhou; tentando a API diretamente");
            }

            bool login_ok = false;
            if (MutexHTTP == NULL) {
                ESP_LOGW(TAG, "MutexHTTP nulo; login sem mutex");
                login_ok = fazer_login(userNameHTTPs, passwordHTTPs);
            } else if (xSemaphoreTake(MutexHTTP, pdMS_TO_TICKS(HTTP_MUTEX_WAIT_MS)) == pdTRUE) {
                login_ok = fazer_login(userNameHTTPs, passwordHTTPs);
                xSemaphoreGive(MutexHTTP);
            } else {
                ESP_LOGW(TAG, "Login aguardando HTTP livre; MutexHTTP ocupado");
            }

            if (login_ok) {
                ESP_LOGI(TAG, "Login bem-sucedido. Token");
                TokenOk = 1;

                if (taskInit == 0) {
                    taskInit = 1;
                    // atualizacao_OTA();

                    // snprintf(mensagemRST, sizeof(mensagemRST), "Motivo do último reset: %s ",
                    // reset_reason_str(reset_reason)); alertApiEvents(mensagemRST); alertApiEvents("O Connect
                    // estabeleceu conexão com a rede.");
                }

#if (GTW_ROLE_RX_ONLY == 0)
                if (taskConnect_to_websocket == NULL) {
                    xTaskCreate(connect_to_websocket, "connect_to_websocket", 2048 * 6, NULL, 5,
                                &taskConnect_to_websocket);
                } else {
                    ESP_LOGW(TAG, "Task de WebSocket já está em execução.");
                    stop_websocket_task = true;
                    vTaskDelay(pdMS_TO_TICKS(15000));
#if DEBUG_MODE
                    ESP_LOGE("Login", "Reiniciando a task de WebSocket...\n");
#endif

                    if (taskConnect_to_websocket == NULL) {
                        xTaskCreate(connect_to_websocket, "connect_to_websocket", 2048 * 6, NULL, 5,
                                    &taskConnect_to_websocket);
                    }
                }

                // Em vez de delay de 20h → espera notificação do timer
#else
                ESP_LOGI(TAG, "Modo RX_ONLY: WebSocket desativado; login mantido para PATCH");
#endif

#if (GTW_ROLE_TX_ONLY == 0)
                while (task_patch == NULL) {
#if (GTW_ROLE_RX_ONLY == 0)
                    for (int i = 0; i < 80 && taskConnect_to_websocket != NULL; i++) {
                        if (ws_client != NULL && esp_websocket_client_is_connected(ws_client)) {
                            break;
                        }
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }

#endif
                    if (xTaskCreate(patch_task, "patch_task", 1024 * 8, NULL, 5, &task_patch) != pdPASS) {
                        task_patch = NULL;
                        ESP_LOGE(TAG, "Falha ao criar patch_task; tentando novamente em 5s");
                        vTaskDelay(pdMS_TO_TICKS(5000));
                    }
                }
#else
                ESP_LOGI(TAG, "Modo TX_ONLY: patch_task desativada");
#endif

                while (!restart_token_timer()) {
                    ESP_LOGW(TAG, "Timer do token indisponivel; nova tentativa em 5s");
                    vTaskDelay(pdMS_TO_TICKS(5000));
                }

                uint32_t notified;
                xTaskNotifyWait(0, UINT32_MAX, &notified, portMAX_DELAY);
                ESP_LOGI(TAG, "20h passaram → renovando token...");
                continue;
            } else {
                ESP_LOGW(TAG, "Login falhou, tentando novamente em 5s.");
                vTaskDelay(pdMS_TO_TICKS(5000)); // 5s
            }
        } else {
            ESP_LOGI(TAG, "Aguardando Wi-Fi e internet...");
            vTaskDelay(pdMS_TO_TICKS(10000)); // 10s
        }
    }
}

/*############################################## WebSocket ################################################*/

// Função principal da tarefa de conexão WebSocket
#if (GTW_ROLE_RX_ONLY == 0)
void connect_to_websocket(void *pvParameters) {

    static char tmp[900];
    static char ws_path[800];
    stop_websocket_task = false;

    snprintf(ws_path, sizeof(ws_path), "/ws-native/native-ws?token=%s", token_global);
    snprintf(tmp, sizeof(tmp), "wss://jalles.aclsconnect.com%s", ws_path);
    // snprintf(tmp, sizeof(tmp),
    // "wss://83cd1aa837661fab941b2c5a2a65424b.jm.net.br:2087/ws-native/native-ws?token=%s",token_global); snprintf(tmp,
    // sizeof(tmp), "ws://192.168.1.111:8017/native-ws?token=%s", token_global);

    // Duplica pra heap (memória estável)
    websocket_url = strdup(tmp);

    if (!websocket_url) {
        ESP_LOGE("WS", "Sem memória pra URL do websocket");
        vTaskDelete(NULL);
        return;
    }

    gtw_websocket_transport_t network_transport = {0};
    esp_err_t transport_err = gtw_websocket_transport_init(&network_transport, ws_path, rootCaCerticate);
    if (transport_err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao preparar transporte WebSocket %s: %s", GTW_IP_VERSION_NAME,
                 esp_err_to_name(transport_err));
        free(websocket_url);
        websocket_url = NULL;
        taskConnect_to_websocket = NULL;
        vTaskDelete(NULL);
        return;
    }

    esp_websocket_client_config_t websocket_cfg = {
        .uri = websocket_url,
        .ext_transport = network_transport.websocket,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 30000,
        // .keep_alive_enable = true,
        // .disable_auto_reconnect = false,
        // .ping_interval_sec = 10,
        // .pingpong_timeout_sec = 5,
    };

    // esp_websocket_client_handle_t websocket_client = esp_websocket_client_init(&websocket_cfg);

    ws_client = esp_websocket_client_init(&websocket_cfg);

    if (ws_client == NULL) {
        ESP_LOGE(TAG, "Falha ao inicializar o cliente WebSocket");
        if (websocket_url) {
            free(websocket_url);
            websocket_url = NULL;
        }
        gtw_websocket_transport_cleanup(&network_transport);
        taskConnect_to_websocket = NULL;
        vTaskDelete(NULL);
        return;
    }

    esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)ws_msg_queue);

    if (esp_websocket_client_start(ws_client) != ESP_OK) {
        ESP_LOGE(TAG, "Erro ao iniciar o cliente WebSocket");
        esp_websocket_client_destroy(ws_client);
        gtw_websocket_transport_cleanup(&network_transport);
        ws_client = NULL;
        if (websocket_url) {
            free(websocket_url);
            websocket_url = NULL;
        }
        taskConnect_to_websocket = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Tentando conectar ao WebSocket...");
    int reconnect_attempts = 0;

    while (!stop_websocket_task) {
        if (!esp_websocket_client_is_connected(ws_client)) {
            if (reconnect_attempts < MAX_RECONNECT_ATTEMPTS) {
                reconnect_attempts++;
                ESP_LOGW(TAG, "Tentando reconectar ao WebSocket... Tentativa %d", reconnect_attempts);
                vTaskDelay(pdMS_TO_TICKS(5000));
            } else {
                ESP_LOGE(TAG, "Máximo de tentativas de reconexão atingido. Finalizando...");
                break;
            }
        } else {
            reconnect_attempts = 0;
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    ESP_LOGI(TAG, "Encerrando WebSocket...");

    esp_websocket_client_stop(ws_client);
    esp_websocket_client_destroy(ws_client);
    ws_client = NULL;
    gtw_websocket_transport_cleanup(&network_transport);

    if (websocket_url) {
        free(websocket_url);
        websocket_url = NULL;
    }

    taskConnect_to_websocket = NULL;
    vTaskDelete(NULL);
}

static void websocket_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    QueueHandle_t q = (QueueHandle_t)arg;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    if (!q || !data)
        return;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
#if DEBUG_MODE
        ESP_LOGI(TAG_Websocket, "WebSocket conectado");
#endif
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
#if DEBUG_MODE
        ESP_LOGI(TAG_Websocket, "WebSocket desconectado");
#endif
        stop_websocket_task = true;
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x1) {
            ESP_LOGI("Websoket", "______________________mensagem\n");
            // texto
            if (data->data_len == 0 || data->data_len >= WS_MSG_MAX_LEN) {
#if DEBUG_MODE
                ESP_LOGE(TAG_Websocket, "Mensagem muito grande: %d bytes", data->data_len);
#endif
                break;
            }

            ws_msg_item_t item = {0};

            // aloca buffer
            item.data = malloc(data->data_len + 1);
            if (!item.data) {
                ESP_LOGE(TAG_Websocket, "Sem memória p/ buffer WS");
                return;
            }

            memcpy(item.data, data->data_ptr, data->data_len);
            item.data[data->data_len] = '\0';
            item.len = data->data_len;

            if (xQueueSend(q, &item, 0) != pdTRUE) {
                ESP_LOGW(TAG_Websocket, "Fila WS cheia — mensagem descartada");
                free(item.data);
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
#if DEBUG_MODE
        ESP_LOGE(TAG_Websocket, "Erro no WebSocket");
#endif
        break;

    default:
        break;
    }
}

static void ws_msg_processor_task(void *pv) {
    ws_msg_item_t item;
    while (1) {
        if (xQueueReceive(ws_msg_queue, &item, pdMS_TO_TICKS(2000)) == pdTRUE) {

#if DEBUG_MODE
            ESP_LOGI("Websoket", "______________________Mensagem WS recebida: %s\n", item.data);
#endif
            cJSON *json = cJSON_Parse(item.data);

            // liberar o buffer da mensagem sempre
            if (!json) {
                ESP_LOGW(TAG_Websocket, "JSON inválido recebido");
                free(item.data); // ✅ libera aqui
                continue;
            }
            cJSON *event_item = cJSON_GetObjectItemCaseSensitive(json, "event");
            if (!event_item || !cJSON_IsString(event_item)) {
                cJSON_Delete(json);
                free(item.data);
                continue;
            }

            const char *event_type = event_item->valuestring;

            if (strcmp(event_type, "reset_gtw") == 0) {
                // handle_reset_command(json);
            } else if (strcmp(event_type, "ControlerBomba_edit") == 0) {
                handle_bomba_control(json);

                ESP_LOGI(TAG_Websocket, "Comando de controle de bomba recebido");
            } else if (strcmp(event_type, "PingGtw") == 0) {
                handle_PingTanque(json);

                ESP_LOGI(TAG_Websocket, "Ping recebido");
            } else if (strcmp(event_type, "BtON_gateway") == 0) {
                handle_BTOn(json);

                ESP_LOGI(TAG_Websocket, "BT recebido");
            } else {
                ESP_LOGW(TAG_Websocket, "Evento desconhecido: %s", event_type);
            }

            cJSON_Delete(json);
            free(item.data);
        }
        // else timeout: volta checando stop flag
    }

    ESP_LOGI(TAG_Websocket, "ws_msg_processor_task finalizando");
    vTaskDelete(NULL);
}

static void handle_bomba_control(cJSON *json) {

    cJSON *data_obj = cJSON_GetObjectItemCaseSensitive(json, "data");
    if (data_obj == NULL) {
#if DEBUG_MODE
        ESP_LOGE(TAG_Websocket, "Erro: 'data' não encontrado no JSON");
#endif
        return;
    }

    cJSON *idBomba = cJSON_GetObjectItemCaseSensitive(data_obj, "bombaId");
    cJSON *idControle = cJSON_GetObjectItemCaseSensitive(data_obj, "id");
    cJSON *comando = cJSON_GetObjectItemCaseSensitive(data_obj, "comando");
    cJSON *status = cJSON_GetObjectItemCaseSensitive(data_obj, "stausBOmba");
    cJSON *gtwId = cJSON_GetObjectItemCaseSensitive(data_obj, "gtwId");
    cJSON *tanqueId = cJSON_GetObjectItemCaseSensitive(data_obj, "tanqueId");
    cJSON *vazao = cJSON_GetObjectItemCaseSensitive(data_obj, "vazao");

    if (!cJSON_IsNumber(idBomba) || !cJSON_IsBool(comando) || !cJSON_IsNumber(status) || !cJSON_IsNumber(gtwId) ||
        !cJSON_IsNumber(tanqueId)) {
#if DEBUG_MODE
        ESP_LOGE(TAG_Websocket, "Campos 'bombaId' ou 'comando' ausentes/inválidos");
#endif
        return;
    }

    if (gtwId->valueint != DEVICE_ID) {
#if DEBUG_MODE
        ESP_LOGW(TAG_Websocket, "Comando ignorado: destino GTW=%d, este GTW=%d", gtwId->valueint, DEVICE_ID);
#endif
        return;
    }

    // se vazao não vier, usa 0
    float valor_vazao = 0.0f;
    if (vazao && cJSON_IsNumber(vazao)) {
        valor_vazao = (float)vazao->valuedouble;
    }
    int controle_id = cJSON_IsNumber(idControle) ? idControle->valueint : idBomba->valueint;

    ESP_LOGI(TAG_Websocket, "Controlando bomba ID %d: comando=%s", idBomba->valueint,
             status->valueint ? "DESLIGAR" : "LIGAR");
    ESP_LOGI(TAG_Websocket, "Para tanque ID %d via gateway ID %d", tanqueId->valueint, gtwId->valueint);

    float Firm_ver_api = -3;
    char URL_OTA[80];

    if (cJSON_IsNumber(tanqueId)) {
        int id = tanqueId->valueint;

        snprintf(URL_OTA, sizeof(URL_OTA), "%s%s%d", MAIN_ROUTE, leituraGtw, id);

        ESP_LOGI("Comando Web", "Comando bomba : %s", URL_OTA);

        char *json_buffer = NULL;
        int json_len = 0;

        int ok = 0;
        if (xSemaphoreTake(MutexHTTP, portMAX_DELAY) == pdTRUE) {
            ok = server_get_json(URL_OTA, &json_buffer, &json_len);
            xSemaphoreGive(MutexHTTP);
        } else {
            ESP_LOGE("Comando Web", "Nao foi possivel obter MutexHTTP para consultar tanque %d", id);
            return;
        }

        if (ok != 1 || json_buffer == NULL || json_len <= 0) {
            ESP_LOGE("Comando Web", "Falha ao consultar tanque %d (ok=%d, len=%d)", id, ok, json_len);
            free(json_buffer);
            return;
        }

        cJSON *Json_GTW = cJSON_Parse(json_buffer); // Salva o Json que foi pego na requisicao HTTP

        if (Json_GTW == NULL) {
            ESP_LOGE("Comando Web", "Erro no cJson Parse");
            free(json_buffer);
            return;
        }

        ESP_LOGI("Comando Web", "Json recebido: %s", json_buffer);
        free(json_buffer);
        json_buffer = NULL;

        cJSON *online = cJSON_GetObjectItemCaseSensitive(Json_GTW, "online");

        if (online && cJSON_IsBool(online)) {

            if (cJSON_IsTrue(online)) {
                ESP_LOGI("Comando Web", "Tanque ONLINE");
                // coloque sua lógica para online aqui
            } else {
                ESP_LOGW("Comando Web", "Tanque OFFLINE");
                // coloque sua lógica para offline aqui
                char msg[64]; // tamanho suficiente
                snprintf(msg, sizeof(msg), "{\"s\":%d,\"v\":%.2f,\"c\":%d}", status->valueint, valor_vazao,
                         controle_id);
                gtw_send_to_tank(tanqueId->valueint, msg, idBomba->valueint);
            }
        } else {
            ESP_LOGE("Comando Web", "Campo 'online' inválido ou inexistente");
        }

        cJSON_Delete(Json_GTW);
    }
}

static void handle_PingTanque(cJSON *json) {

    cJSON *data_obj = cJSON_GetObjectItemCaseSensitive(json, "data");
    if (data_obj == NULL) {
#if DEBUG_MODE
        ESP_LOGE(TAG_Websocket, "Erro: 'data' não encontrado no JSON");
#endif
        return;
    }

    cJSON *gtwId = cJSON_GetObjectItemCaseSensitive(data_obj, "gtw_tanque");
    cJSON *tanqueId = cJSON_GetObjectItemCaseSensitive(data_obj, "id");

    if (!cJSON_IsNumber(gtwId) || !cJSON_IsNumber(tanqueId)) {
#if DEBUG_MODE
        ESP_LOGE(TAG_Websocket, "Campos 'tanqueId' ou 'gtwId' ausentes/inválidos");
#endif
        return;
    }

    printf("Ping recebido para tanque ID %d via gateway ID %d\n", tanqueId->valueint, gtwId->valueint);
    printf("Comparando com ID deste gateway: %d\n", DEVICE_ID);

    if (gtwId->valueint != DEVICE_ID) {
#if DEBUG_MODE
        ESP_LOGW(TAG_Websocket, "Ping ignorado: destino GTW=%d, este GTW=%d", gtwId->valueint, DEVICE_ID);
#endif
        return;
    }

    ESP_LOGI(TAG_Websocket, "Ping para tanque ID %d via gateway ID %d", tanqueId->valueint, gtwId->valueint);

    bool pingPong = gtw_ping_tank(tanqueId->valueint);

    ESP_LOGI(TAG_Websocket, "Ping para tanque ID %d: %s", tanqueId->valueint, pingPong ? "SUCESSO" : "FALHA");

    char msg[200];
    snprintf(msg, sizeof(msg), "{\"type\":\"ping_result\",\"tankId\":%d,\"status\":\"%s\"}", tanqueId->valueint,
             pingPong ? "success" : "fail");

    ws_send_json(msg);
}

static void handle_BTOn(cJSON *json) {
    cJSON *data_obj = cJSON_GetObjectItemCaseSensitive(json, "data");
    if (!cJSON_IsObject(data_obj)) {
        ESP_LOGE(TAG_Websocket, "'data' inválido");
        return;
    }

    cJSON *gtwId = cJSON_GetObjectItemCaseSensitive(data_obj, "id");
    if (!cJSON_IsNumber(gtwId)) {
        ESP_LOGE(TAG_Websocket, "'id' inválido ou não numérico");
        return;
    }

    int received_id = gtwId->valueint;

    if (received_id != DEVICE_ID) {
        ESP_LOGI(TAG_Websocket, "Mensagem não é para este gateway (%d)", received_id);
        return;
    }

    printf("Comando BLE recebido para gateway ID %d\n", received_id);

    esp_err_t err = bluetooth_config_start(BLE_STARTUP_WINDOW_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_Websocket, "Falha ao abrir BLE: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG_Websocket, "BLE aberto por 1 minuto para configuracao");
}

bool ws_send_json(const char *json_msg) {
    if (!ws_client) {
        ESP_LOGE("WS", "WebSocket client NULL");
        return false;
    }

    if (!esp_websocket_client_is_connected(ws_client)) {
        ESP_LOGE("WS", "WebSocket não conectado");
        return false;
    }

    int len = strlen(json_msg);

    int sent = esp_websocket_client_send_text(ws_client, json_msg, len, pdMS_TO_TICKS(5000));

    if (sent < 0) {
        ESP_LOGE("WS", "Falha ao enviar mensagem WebSocket");
        return false;
    }

    ESP_LOGI("WS", "Mensagem enviada WS: %s", json_msg);
    return true;
}
#endif

/*######################################### Fila De Patch ############################################*/

#if (GTW_ROLE_TX_ONLY == 0)
static bool is_tank_level_patch(const char *path, const char *body, http_method_t method) {
    return method == HTTP_PATCH && path && body && strncmp(path, "/tanque/", 8) == 0 &&
           strstr(body, "nivel_atual") != NULL;
}

static bool coalesce_queued_tank_level_patch(const char *path, const char *body, http_method_t method) {
    if (!patch_pool_mutex || !path || !body) {
        return false;
    }

    if (!is_tank_level_patch(path, body, method)) {
        return false;
    }

    if (xSemaphoreTake(patch_pool_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }

    for (int i = 0; i < QUEUE_LENGTH; i++) {
        PatchRequest *request = &patchPool[i];

        if (patchPoolState[i] == PATCH_SLOT_QUEUED && request->method == method && strcmp(request->path, path) == 0) {
            strncpy(request->body, body, BODY_SIZE - 1);
            request->body[BODY_SIZE - 1] = '\0';
            xSemaphoreGive(patch_pool_mutex);
            ESP_LOGW("HTTP_QUEUE", "PATCH nivel coalescido para %s; mantendo apenas valor mais novo", path);
            return true;
        }
    }

    xSemaphoreGive(patch_pool_mutex);
    return false;
}

static void patch_slot_set(uint8_t idx, patch_slot_state_t state) {
    if (!patch_pool_mutex || idx >= QUEUE_LENGTH) {
        return;
    }

    if (xSemaphoreTake(patch_pool_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        patchPoolState[idx] = state;
        xSemaphoreGive(patch_pool_mutex);
    }
}

static bool queue_http_request(const char *path, const char *body, http_method_t method) {
    if (!path || !body) {
        return false;
    }

    if (strlen(path) >= PATH_SIZE || strlen(body) >= BODY_SIZE) {
        ESP_LOGE("HTTP_QUEUE", "Mensagem excede o tamanho da fila");
        return false;
    }

    bool tank_level_patch = is_tank_level_patch(path, body, method);

    if (tank_level_patch && coalesce_queued_tank_level_patch(path, body, method)) {
        return true;
    }

    uint8_t idx;
    if (xQueueReceive(xPatchFreeQueue, &idx, pdMS_TO_TICKS(100)) != pdPASS) {
        if (tank_level_patch) {
            if (coalesce_queued_tank_level_patch(path, body, method)) {
                return true;
            }
            ESP_LOGW("HTTP_QUEUE", "Fila cheia; nivel de %s nao enfileirado e sem ACK", path);
            return false;
        }

        ESP_LOGW("HTTP_QUEUE", "Fila cheia, mensagem LoRa nao confirmada");
        return false;
    }

    PatchRequest *request = &patchPool[idx];
    strncpy(request->path, path, PATH_SIZE - 1);
    request->path[PATH_SIZE - 1] = '\0';
    strncpy(request->body, body, BODY_SIZE - 1);
    request->body[BODY_SIZE - 1] = '\0';
    request->method = method;
    patch_slot_set(idx, PATCH_SLOT_QUEUED);

    if (xQueueSend(xPatchQueue, &request, pdMS_TO_TICKS(500)) != pdPASS) {
        ESP_LOGE("HTTP_QUEUE", "Falha ao enfileirar requisicao");
        patch_slot_set(idx, PATCH_SLOT_FREE);
        xQueueSend(xPatchFreeQueue, &idx, 0);
        return false;
    }

    return true;
}

bool queue_patch_request(const char *path, const char *body) { return queue_http_request(path, body, HTTP_PATCH); }

static uint32_t patch_backoff_delay_ms(uint32_t fail_count) {
    uint32_t delay_ms = PATCH_BACKOFF_MIN_MS;

    for (uint32_t i = 1; i < fail_count && delay_ms < PATCH_BACKOFF_MAX_MS; i++) {
        delay_ms *= 2;
        if (delay_ms > PATCH_BACKOFF_MAX_MS) {
            delay_ms = PATCH_BACKOFF_MAX_MS;
        }
    }

    return delay_ms + (esp_random() % PATCH_BACKOFF_JITTER_MS);
}

static bool patch_status_is_permanent_client_error(int status) {
    return status >= 400 && status < 500 && status != 401 && status != 403 && status != 408 && status != 429;
}

// Tarefa para processar a fila
void patch_task(void *pvParameters) {
    PatchRequest *request;
    uint32_t mutex_fail_count = 0;

    while (1) {
        if (xQueueReceive(xPatchQueue, &request, portMAX_DELAY) == pdTRUE) {
            uint8_t idx = (uint8_t)(request - patchPool);
            int status = -1;
            bool drop_request = false;
            bool requeue_request = false;
            uint32_t request_attempts = 0;
            patch_slot_set(idx, PATCH_SLOT_PROCESSING);

            while (!drop_request && (status < 200 || status >= 300)) {
#if (GTW_ROLE_RX_ONLY == 0)
                if (ws_client != NULL && !esp_websocket_client_is_connected(ws_client)) {
                    ESP_LOGW("PATCH_TASK", "WebSocket conectando; aguardando antes do PATCH");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    continue;
                }
#endif

                if (xSemaphoreTake(MutexHTTP, pdMS_TO_TICKS(12000)) == pdTRUE) {
                    mutex_fail_count = 0;
                    request_attempts++;
                    status = server_request(request->path, request->body, request->method);
                    xSemaphoreGive(MutexHTTP);

                    if (status >= 200 && status < 300) {
                        break;
                    }

                    if (patch_status_is_permanent_client_error(status)) {
                        ESP_LOGE("PATCH_TASK", "Descartando requisicao %s: erro HTTP permanente status=%d body=%s",
                                 request->path, status, request->body);
                        drop_request = true;
                        break;
                    }

                    ESP_LOGW("PATCH_TASK", "Falha HTTP tentativa %u/%u, status=%d", (unsigned)request_attempts,
                             (unsigned)PATCH_MAX_ATTEMPTS, status);

                    if (request_attempts >= PATCH_MAX_ATTEMPTS) {
                        ESP_LOGW("PATCH_TASK",
                                 "Recolocando requisicao no fim da fila apos %u tentativas: status=%d path=%s",
                                 (unsigned)request_attempts, status, request->path);
                        requeue_request = true;
                        break;
                    }

#if (GTW_ROLE_RX_ONLY == 0)
                    if (status == -1 && ws_client != NULL && esp_websocket_client_is_connected(ws_client)) {
                        ESP_LOGW("PATCH_TASK", "Falha TLS/HTTP com WebSocket ativo; mantendo WebSocket ligado");
                    }
#endif
                } else {
                    mutex_fail_count++;
                    ESP_LOGW("PATCH_TASK", "MutexHTTP timeout (%d)", mutex_fail_count);
                    if (mutex_fail_count >= 5)
                        ConnectRest();
                }

                uint32_t backoff_ms = patch_backoff_delay_ms(request_attempts + mutex_fail_count);
                ESP_LOGW("PATCH_TASK", "Nova tentativa HTTP em %u ms", (unsigned)backoff_ms);
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            }

            if (requeue_request) {
                patch_slot_set(idx, PATCH_SLOT_QUEUED);

                if (xQueueSend(xPatchQueue, &request, pdMS_TO_TICKS(500)) != pdPASS) {
                    ESP_LOGE("PATCH_TASK", "Falha ao recolocar requisicao na fila; liberando slot path=%s",
                             request->path);
                    patch_slot_set(idx, PATCH_SLOT_FREE);
                    xQueueSend(xPatchFreeQueue, &idx, 0);
                }
            } else {
                patch_slot_set(idx, PATCH_SLOT_FREE);
                xQueueSend(xPatchFreeQueue, &idx, 0);
            }
        }
    }
}

#endif

/*############################################## Lora  ################################################*/

static void lora_setup_gtw(void) {
    msg_counter_gtw = (uint8_t)esp_random();

    lora_e32_config_t cfg = {
        .uart_num = UART_NUM_1,
        .tx_pin = 16,
        .rx_pin = 18,
        .m0_pin = 12,
        .m1_pin = 14,
        // E32-900T30S não possui RESET dedicado.
        .rst_pin = -1,
        .uart_baudrate = 9600,

        // exatamente o que seu teste que funcionou usa:
        .head = 0xC0,
        .addh = 0x00,
        .addl = 0x01,
        .speed = 0x18, // configuracao legada usada pelos tanques instalados em campo
        .channel = 0x17,
        .option = 0x64,
    };

    esp_err_t err = lora_e32_init(&cfg);
    if (err == ESP_OK) {
        err = lora_e32_apply_cfg();
    }

    gtw_lora_ready = (err == ESP_OK);
    if (!gtw_lora_ready) {
        ESP_LOGE("LORA_SETUP", "E32 indisponivel no boot: %s; recuperacao seguira em segundo plano",
                 esp_err_to_name(err));
    }
}

void gtw_lora_rx_task(void *pvParameters) {
    lora_app_frame_t rx;
    gtw_lora_rx_stats_t stats = {0};
    int64_t last_stats_log_ms = esp_timer_get_time() / 1000;

#if (GTW_ROLE_TX_ONLY != 0)
    ESP_LOGI(TAG, "LoRa TX_ONLY: escutando ACKs dos tanques (ID=%d)", DEVICE_ID);
#else
    ESP_LOGI(TAG, "LoRa RX_ONLY: recebendo telemetria dos tanques para PATCH (ID=%d)", DEVICE_ID);
#endif
    esp_task_wdt_add(NULL);

    while (1) {
        esp_task_wdt_reset();

        if (!gtw_lora_ready) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int len = 0;
        memset(&rx, 0, sizeof(rx));
        int64_t now_ms = esp_timer_get_time() / 1000;

        if ((now_ms - last_stats_log_ms) >= 60000) {
#if DEBUG_MODE
            gtw_lora_log_rx_stats(&stats, "periodico_60s");
#endif
            last_stats_log_ms = now_ms;
        }

        if (xSemaphoreTake(MutexLora, pdMS_TO_TICKS(GTW_LORA_MUTEX_WAIT_MS)) == pdTRUE) {
            len = lora_e32_receive_raw((uint8_t *)&rx, sizeof(rx), GTW_LORA_RX_TIMEOUT_MS);
            xSemaphoreGive(MutexLora);
        }

        if (len <= 0) {
            if (len < 0) {
                gtw_lora_ready = false;
                ESP_LOGE("LORA_HEALTH", "Falha UART ao receber; E32 marcado para recuperacao");
            }
            stats.timeout++;
            if ((stats.timeout % 500U) == 0U)
                gtw_lora_log_rx_stats(&stats, "timeout_500");
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        stats.frame_ok++;
#if DEBUG_MODE
        ESP_LOGI(TAG,
                 "RX frame bruto ok #%lu bytes=%d src_type=%u src_id=%u dst_type=%u dst_id=%u type=%u msg_id=%u len=%u",
                 (unsigned long)stats.frame_ok, len, rx.src_type, rx.src_id, rx.dst_type, rx.dst_id, rx.msg_type,
                 rx.msg_id, rx.len);
#endif

        size_t expected_air_len = lora_frame_air_len(&rx);
        if (expected_air_len == 0 || len != (int)expected_air_len) {
            stats.incomplete++;
            ESP_LOGW(TAG, "Frame LoRa incompleto: %d/%u", len, (unsigned)expected_air_len);
            gtw_lora_log_rx_stats(&stats, "incomplete");
            continue;
        }

#if DEBUG_MODE
        ESP_LOGI(TAG, "RX src=%d id=%d type=%d msg=%.*s", rx.src_type, rx.src_id, rx.msg_type, rx.len, rx.payload);
#endif

        if (rx.preamble != LORA_PREAMBLE) {
            stats.bad_preamble++;
            gtw_lora_log_rx_stats(&stats, "bad_preamble");
            printf("Preamble inválido: 0x%02X\n", rx.preamble);
            continue;
        }

        uint16_t crc_calc = 0;
        uint16_t crc_rx = 0;

        if (!lora_received_frame_valid(&rx, len, &crc_calc, &crc_rx)) {
            stats.bad_crc++;
            gtw_lora_log_rx_stats(&stats, "bad_crc");
            ESP_LOGW(TAG, "CRC inválido");
            continue;
        }

        if (rx.len > sizeof(rx.payload) ||
            (rx.src_type != DEV_TANK && rx.src_type != DEV_GTW && rx.src_type != DEV_REP) || rx.msg_type < MSG_DATA ||
            rx.msg_type > MSG_PING) {
            stats.semantic_invalid++;
            gtw_lora_log_rx_stats(&stats, "semantic_invalid");
            ESP_LOGW(TAG, "Frame LoRa semanticamente invalido");
            continue;
        }

        if (rx.dst_type != DEV_GTW || rx.dst_id != DEVICE_ID) {
            stats.wrong_dst++;
            gtw_lora_log_rx_stats(&stats, "wrong_dst");
            ESP_LOGW(TAG, "Frame não é para mim");
            continue;
        }

        gtw_last_lora_rx_ms = esp_timer_get_time() / 1000;
        if (rx.src_type == DEV_TANK)
            gtw_mark_tank_seen(rx.src_id);

#if (GTW_ROLE_RX_ONLY == 0)
        TaskHandle_t ping_waiter = NULL;
        xSemaphoreTake(gtw_ack_mutex, portMAX_DELAY);
        if (gtw_ack_wait.active && gtw_ack_wait.accept_any_frame && gtw_ack_wait.tank_id == rx.src_id &&
            rx.src_type == DEV_TANK) {
            ping_waiter = gtw_ack_wait.waiter;
            gtw_ack_wait.active = false;
            gtw_ack_wait.accept_any_frame = false;
            gtw_ack_wait.waiter = NULL;
        }
        xSemaphoreGive(gtw_ack_mutex);

        if (ping_waiter)
            xTaskNotifyGive(ping_waiter);

        if (rx.msg_type == MSG_ACK) {
            stats.ack_rx++;
            TaskHandle_t waiter = NULL;

            xSemaphoreTake(gtw_ack_mutex, portMAX_DELAY);
            if (gtw_ack_wait.active && gtw_ack_wait.msg_id == rx.msg_id && gtw_ack_wait.tank_id == rx.src_id) {
                waiter = gtw_ack_wait.waiter;
                gtw_ack_wait.active = false;
                gtw_ack_wait.accept_any_frame = false;
                gtw_ack_wait.waiter = NULL;
            }
            xSemaphoreGive(gtw_ack_mutex);

            if (waiter) {
                stats.ack_match++;
                xTaskNotifyGive(waiter);
            } else {
                stats.ack_unexpected++;
            }
            continue;
        }
#else
        if (rx.msg_type == MSG_ACK) {
            stats.ack_unexpected++;
            continue;
        }
#endif

#if (GTW_ROLE_TX_ONLY != 0)
        ESP_LOGI(TAG, "Modo TX_ONLY: frame LoRa nao-ACK ignorado src=%u type=%u msg_id=%u", rx.src_id, rx.msg_type,
                 rx.msg_id);
        continue;
#else

        if (rx.src_type == DEV_TANK && rx.msg_type == MSG_PING) {
            stats.ping_rx++;
            gtw_send_ack(&rx);
            stats.ack_sent++;
#if DEBUG_MODE
            ESP_LOGI(TAG, "Heartbeat LoRa recebido do tanque %u", rx.src_id);
#endif
            continue;
        }

        if (rx.src_type == DEV_TANK && rx.msg_type == MSG_DATA) {
            stats.data_rx++;
            TickType_t now = xTaskGetTickCount();
            gtw_rx_dup_t *dup = &gtw_rx_dup_cache[(rx.src_id ^ rx.msg_id) % GTW_RX_DUP_CACHE_SIZE];

            if (dup->used && dup->src_id == rx.src_id && dup->msg_id == rx.msg_id &&
                (now - dup->accepted_at) < pdMS_TO_TICKS(GTW_LORA_DUPLICATE_WINDOW_MS)) {
                stats.duplicate++;
                gtw_send_ack(&rx);
                stats.ack_sent++;
                gtw_lora_log_rx_stats(&stats, "duplicate_ack");
                continue;
            }

            size_t body_len = rx.len;
            if (body_len > sizeof(rx.payload))
                body_len = sizeof(rx.payload);

            char body[sizeof(rx.payload) + 1] = {0};
            memcpy(body, rx.payload, body_len);
            body[body_len] = '\0';

            bool accepted = false;

            if (rx.has_bomba_id == 1) {
#if DEBUG_MODE
                printf("Enviando PATCH para BOMBA %d (Tanque %d)\n", rx.bomba_id, rx.src_id);
#endif

                char pathBomba[40];
                snprintf(pathBomba, sizeof(pathBomba), "/bomba/%d", rx.bomba_id);

                char expanded_body[BODY_SIZE];
                const char *patch_body = body;
                if (gtw_expand_short_lora_body(body, rx.has_bomba_id, expanded_body, sizeof(expanded_body))) {
                    patch_body = expanded_body;
#if DEBUG_MODE
                    ESP_LOGI(TAG, "Payload curto expandido: %s", patch_body);
#endif
                }

                accepted = queue_patch_request(pathBomba, patch_body);
            } else if (rx.has_bomba_id == 0) {
#if DEBUG_MODE
                printf("Enviando PATCH para TANQUE %d\n", rx.src_id);
#endif

                char pathTanque[30];
                snprintf(pathTanque, sizeof(pathTanque), "/tanque/%d", rx.src_id);

                char expanded_body[BODY_SIZE];
                const char *patch_body = body;
                if (gtw_expand_short_lora_body(body, rx.has_bomba_id, expanded_body, sizeof(expanded_body))) {
                    patch_body = expanded_body;
#if DEBUG_MODE
                    ESP_LOGI(TAG, "Payload curto expandido: %s", patch_body);
#endif
                }

                accepted = queue_patch_request(pathTanque, patch_body);
            } else if (rx.has_bomba_id == 2) {
#if DEBUG_MODE
                printf("Enviando PATCH para Alertas %d\n", rx.src_id);
#endif

                char pathTanque[30];
                snprintf(pathTanque, sizeof(pathTanque), "/alertas/tanque");

                char expanded_alert[BODY_SIZE];
                const char *alert_body = body;

                if (gtw_expand_compact_alert_body(body, rx.src_id, expanded_alert, sizeof(expanded_alert))) {
                    alert_body = expanded_alert;
#if DEBUG_MODE
                    ESP_LOGI(TAG, "Alerta compacto expandido: %s", alert_body);
#endif
                }

                accepted = queue_http_request(pathTanque, alert_body, HTTP_POST);
            } else if (rx.has_bomba_id == 3) {
                int controle_id = rx.bomba_id;

                cJSON *root = cJSON_Parse(body);
                if (root) {
                    cJSON *controle = cJSON_GetObjectItemCaseSensitive(root, "c");
                    if (cJSON_IsNumber(controle) && controle->valueint > 0) {
                        controle_id = controle->valueint;
                    }
                    cJSON_Delete(root);
                }

                if (controle_id > 0) {
#if DEBUG_MODE
                    printf("Enviando PATCH para CONTROLE BOMBA %d (Tanque %d)\n", controle_id, rx.src_id);
#endif

                    char pathControle[48];
                    snprintf(pathControle, sizeof(pathControle), "/bomba/controle/%d", controle_id);

                    accepted = queue_patch_request(pathControle, "{\"comando\":false}");
                } else {
                    ESP_LOGW(TAG, "Controle bomba invalido via LoRa: src=%u body=%s", rx.src_id, body);
                }
            }

            if (accepted) {
                stats.queue_accept++;
                dup->used = true;
                dup->src_id = rx.src_id;
                dup->msg_id = rx.msg_id;
                dup->accepted_at = now;
                gtw_send_ack(&rx);
                stats.ack_sent++;
            } else {
                stats.queue_reject++;
                ESP_LOGW(TAG, "Telemetria nao aceita; ACK nao enviado");
                gtw_lora_log_rx_stats(&stats, "queue_reject");
            }
        }
#endif
    }
}

static void gtw_lora_health_task(void *pvParameters) {
    int recovery_failures = 0;
    gtw_last_lora_rx_ms = esp_timer_get_time() / 1000;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));

        int64_t now_ms = esp_timer_get_time() / 1000;
        int64_t silent_ms = now_ms - gtw_last_lora_rx_ms;

        if (gtw_lora_ready && silent_ms < GTW_LORA_SILENCE_RECOVERY_MS) {
            recovery_failures = 0;
            continue;
        }

        ESP_LOGW("LORA_HEALTH", "%s; reinicializando e validando E32",
                 gtw_lora_ready ? "Silencio LoRa prolongado" : "E32 indisponivel");

        if (xSemaphoreTake(gtw_request_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
            esp_err_t err = ESP_ERR_TIMEOUT;

            if (xSemaphoreTake(MutexLora, pdMS_TO_TICKS(5000)) == pdTRUE) {
                err = lora_e32_reinit();
                if (err == ESP_OK) {
                    err = lora_e32_apply_cfg_temp();
                }
                xSemaphoreGive(MutexLora);
            }

            xSemaphoreGive(gtw_request_mutex);

            gtw_lora_ready = (err == ESP_OK);
            if (gtw_lora_ready) {
                recovery_failures = 0;
                ESP_LOGI("LORA_HEALTH", "E32 reinicializado e configuracao confirmada");
            } else {
                recovery_failures++;
                ESP_LOGE("LORA_HEALTH", "Falha ao reconfigurar E32: %s", esp_err_to_name(err));
            }
        }

        gtw_last_lora_rx_ms = now_ms;

        if (recovery_failures >= 5) {
            ESP_LOGE("LORA_HEALTH", "E32 falhou em %d recuperacoes; gateway continuara tentando", recovery_failures);
            recovery_failures = 0;
        }
    }
}

uint16_t lora_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static size_t lora_frame_air_len(const lora_app_frame_t *frame) {
    if (!frame || frame->len > sizeof(frame->payload))
        return 0;

    return offsetof(lora_app_frame_t, payload) + frame->len + sizeof(frame->crc);
}

static int lora_send_frame_air(lora_app_frame_t *frame) {
    if (!frame)
        return -1;

    size_t header_len = offsetof(lora_app_frame_t, payload);
    size_t payload_len = frame->len;
    size_t air_len = lora_frame_air_len(frame);

    if (air_len == 0 || air_len > sizeof(lora_app_frame_t))
        return -1;

    uint8_t raw[sizeof(lora_app_frame_t)] = {0};
    memcpy(raw, frame, header_len + payload_len);

    frame->crc = lora_crc16(raw, header_len + payload_len);
    memcpy(raw + header_len + payload_len, &frame->crc, sizeof(frame->crc));

    return lora_e32_send_raw(raw, (int)air_len);
}

static bool lora_received_frame_valid(const lora_app_frame_t *rx, int air_len, uint16_t *out_crc_calc,
                                      uint16_t *out_crc_rx) {
    if (!rx || air_len < (int)(offsetof(lora_app_frame_t, payload) + sizeof(rx->crc)))
        return false;

    size_t expected_len = lora_frame_air_len(rx);
    if (expected_len == 0 || air_len != (int)expected_len)
        return false;

    const uint8_t *raw = (const uint8_t *)rx;
    uint16_t crc_rx = 0;
    memcpy(&crc_rx, raw + expected_len - sizeof(crc_rx), sizeof(crc_rx));
    uint16_t crc_calc = lora_crc16(raw, expected_len - sizeof(crc_rx));

    if (out_crc_calc)
        *out_crc_calc = crc_calc;
    if (out_crc_rx)
        *out_crc_rx = crc_rx;

    return crc_calc == crc_rx;
}

#if (GTW_ROLE_TX_ONLY == 0)
void gtw_send_ack(const lora_app_frame_t *rx) {
    lora_app_frame_t ack = {0};

    ack.preamble = LORA_PREAMBLE;

    ack.src_type = DEV_GTW;
    ack.src_id = DEVICE_ID;

    ack.dst_type = rx->src_type;
    ack.dst_id = rx->src_id;

    ack.msg_type = MSG_ACK;
    ack.msg_id = rx->msg_id;
    ack.len = 0;

    if (xSemaphoreTake(MutexLora, pdMS_TO_TICKS(2500)) == pdTRUE) {
        size_t air_len = lora_frame_air_len(&ack);
        int sent = lora_send_frame_air(&ack);
        xSemaphoreGive(MutexLora);
        if (sent == (int)air_len) {
#if DEBUG_MODE
            ESP_LOGI(TAG, "ACK enviado id=%d bytes=%d", ack.msg_id, sent);
#endif
        } else {
            ESP_LOGE(TAG, "ACK incompleto id=%d bytes=%d/%u", ack.msg_id, sent, (unsigned)air_len);
        }
    } else {
        ESP_LOGW(TAG, "Nao foi possivel enviar ACK id=%d", ack.msg_id);
    }
}
#endif

#if (GTW_ROLE_RX_ONLY == 0)
static bool gtw_send_frame_wait_ack(lora_app_frame_t *frame) {
    if (!frame || !gtw_request_mutex || !gtw_ack_mutex)
        return false;

    if (xSemaphoreTake(gtw_request_mutex, pdMS_TO_TICKS(15000)) != pdTRUE)
        return false;

    bool success = false;

    for (int attempt = 1; attempt <= ACK_RETRIES; attempt++) {
        ulTaskNotifyTake(pdTRUE, 0);

        xSemaphoreTake(gtw_ack_mutex, portMAX_DELAY);
        gtw_ack_wait.active = true;
        gtw_ack_wait.msg_id = frame->msg_id;
        gtw_ack_wait.tank_id = frame->dst_id;
        gtw_ack_wait.accept_any_frame = (frame->msg_type == MSG_PING);
        gtw_ack_wait.waiter = xTaskGetCurrentTaskHandle();
        xSemaphoreGive(gtw_ack_mutex);

        if (xSemaphoreTake(MutexLora, pdMS_TO_TICKS(2500)) == pdTRUE) {
            size_t air_len = lora_frame_air_len(frame);
            int sent = lora_send_frame_air(frame);
            xSemaphoreGive(MutexLora);
            if (sent != (int)air_len) {
                ESP_LOGE("GTW", "TX LoRa incompleto: %d/%u", sent, (unsigned)air_len);
                xSemaphoreTake(gtw_ack_mutex, portMAX_DELAY);
                gtw_ack_wait.active = false;
                gtw_ack_wait.accept_any_frame = false;
                gtw_ack_wait.waiter = NULL;
                xSemaphoreGive(gtw_ack_mutex);
                continue;
            }
        } else {
            xSemaphoreTake(gtw_ack_mutex, portMAX_DELAY);
            gtw_ack_wait.active = false;
            gtw_ack_wait.accept_any_frame = false;
            gtw_ack_wait.waiter = NULL;
            xSemaphoreGive(gtw_ack_mutex);
            continue;
        }

        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ACK_TIMEOUT_MS)) > 0) {
            success = true;
            break;
        }

        xSemaphoreTake(gtw_ack_mutex, portMAX_DELAY);
        gtw_ack_wait.active = false;
        gtw_ack_wait.accept_any_frame = false;
        gtw_ack_wait.waiter = NULL;
        xSemaphoreGive(gtw_ack_mutex);

        ESP_LOGW("GTW", "Sem ACK do tanque %u, tentativa %d", frame->dst_id, attempt);
        vTaskDelay(pdMS_TO_TICKS(LORA_RETRY_BACKOFF_MIN_MS + (esp_random() % LORA_RETRY_BACKOFF_JITTER_MS)));
    }

    // O E32-900T30S não fornece AUX neste projeto. Mantém uma janela
    // silenciosa antes de liberar o próximo comando/PING para evitar que
    // o pacote seguinte alcance o tanque enquanto ele ainda transmite ACK
    // ou telemetria resultante do comando anterior.
    vTaskDelay(pdMS_TO_TICKS(LORA_TRANSACTION_GUARD_MS));

    xSemaphoreGive(gtw_request_mutex);
    return success;
}

bool gtw_send_to_tank(uint16_t tank_id, const char *msg, uint16_t bomba_id) {
    lora_app_frame_t frame = {0};

    frame.preamble = LORA_PREAMBLE;
    frame.src_type = DEV_GTW;
    frame.src_id = DEVICE_ID;

    frame.dst_type = DEV_TANK;
    frame.dst_id = tank_id;

    if (bomba_id == 0xFFFF || bomba_id == 0) {
        frame.has_bomba_id = 0;
        frame.bomba_id = 0;
    } else {
        frame.has_bomba_id = 1;
        frame.bomba_id = bomba_id;
    }

    frame.msg_type = MSG_CMD;
    frame.msg_id = msg_counter_gtw++;

    frame.len = (uint8_t)strnlen(msg ? msg : "", sizeof(frame.payload));
    if (frame.len > 0)
        memcpy(frame.payload, msg, frame.len);

    bool ok = gtw_send_frame_wait_ack(&frame);
    ESP_LOGI("GTW", "CMD tanque %d: %s", tank_id, ok ? "ACK recebido" : "sem resposta");
    return ok;
}

bool gtw_ping_tank(uint16_t tank_id) {
    if (gtw_tank_seen_recently(tank_id, 10000)) {
        ESP_LOGI("GTW", "PING tanque %d: OK por atividade LoRa recente", tank_id);
        vTaskDelay(pdMS_TO_TICKS(500));
        return true;
    }

    lora_app_frame_t frame = {0};

    frame.preamble = LORA_PREAMBLE;

    frame.src_type = DEV_GTW;
    frame.src_id = DEVICE_ID;

    frame.dst_type = DEV_TANK;
    frame.dst_id = tank_id;

    frame.msg_type = MSG_PING;
    frame.msg_id = msg_counter_gtw++;
    frame.len = 0;

    bool ok = gtw_send_frame_wait_ack(&frame);
    ESP_LOGI("GTW", "PING tanque %d: %s", tank_id, ok ? "OK" : "FALHOU");
    return ok;
}
#endif
/*############################################ BT ############################################*/

static void ble_config_mode_task(void *pvParameters) {
#if (GTW_ROLE_RX_ONLY == 0)
    ESP_LOGW(TAG, "Entrando em modo configuracao BLE: encerrando WebSocket e Wi-Fi");
#else
    ESP_LOGW(TAG, "Entrando em modo configuracao BLE: encerrando Wi-Fi");
#endif

    ble_startup_window_active = false;
    Connectado = 0;

#if (GTW_ROLE_RX_ONLY == 0)
    if (taskConnect_to_websocket != NULL || ws_client != NULL) {
        stop_websocket_task = true;

        for (int i = 0; i < 30 && taskConnect_to_websocket != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (ws_client != NULL) {
            ESP_LOGW(TAG, "WebSocket ainda ativo; solicitando stop para liberar memoria");
            esp_websocket_client_stop(ws_client);
        }
    }
#endif

    if (taskConecta_WIFI != NULL) {
        xTaskNotifyGive(taskConecta_WIFI);
    }

    ESP_LOGI(TAG, "Modo configuracao BLE ativo; desligamento do Wi-Fi solicitado a task responsavel");
    ble_config_mode_task_handle = NULL;
    vTaskDelete(NULL);
}

static void ble_startup_window_task(void *pvParameters) {
    while (ble_startup_window_active) {
        if (!bluetooth_config_is_active()) {
            ble_startup_window_active = false;
            ESP_LOGI(TAG, "Janela BLE do boot encerrada; Wi-Fi sera iniciado pela task");
            if (taskConecta_WIFI != NULL) {
                xTaskNotifyGive(taskConecta_WIFI);
            }
            break;
        }

        if (ble_config_mode_active) {
            ble_startup_window_active = false;
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    vTaskDelete(NULL);
}

void bt_client_connected_callback(void) {
    ble_config_mode_active = true;
    ble_startup_window_active = false;

    if (ble_config_mode_task_handle == NULL) {
        xTaskCreate(ble_config_mode_task, "ble_cfg_mode", 4096, NULL, 6, &ble_config_mode_task_handle);
    }
}

void bt_client_disconnected_callback(void) {
    firmware_ota_ble_abort();
    ble_config_mode_active = false;
    ble_startup_window_active = false;
    ESP_LOGI(TAG, "Saindo do modo configuracao BLE; Wi-Fi sera retomado pela task");
    if (taskConecta_WIFI != NULL) {
        xTaskNotifyGive(taskConecta_WIFI);
    }
}

static void ble_ota_restart_task(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

void bt_binary_received_callback(const uint8_t *data, size_t length) {
    if (!data || length <= 5 || data[0] != 0xA1) {
        bluetooth_send_message("{\"ok\":false,\"type\":\"BleOtaError\",\"error\":\"invalid_packet\"}");
        return;
    }
    uint32_t offset = (uint32_t)data[1] | ((uint32_t)data[2] << 8) | ((uint32_t)data[3] << 16) |
                      ((uint32_t)data[4] << 24);
    uint8_t percent = 0;
    esp_err_t err = firmware_ota_ble_write(offset, data + 5, length - 5, &percent);
    if (err != ESP_OK) {
        char response[128];
        snprintf(response, sizeof(response),
                 "{\"ok\":false,\"type\":\"BleOtaError\",\"error\":\"%s\",\"offset\":%lu}",
                 esp_err_to_name(err), (unsigned long)offset);
        bluetooth_send_message(response);
    }
}

static void bt_send_config_snapshot(void) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        bluetooth_send_message("{\"ok\":false,\"error\":\"no_mem\"}");
        return;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "type", "ConfigDevice");
    cJSON_AddStringToObject(root, "device_role", GTW_ROLE_NAME);
    cJSON_AddNumberToObject(root, "ota_file_id", OTA_FILE_ID);
    cJSON_AddNumberToObject(root, "ota_protocol", 1);
    cJSON_AddStringToObject(root, "firmware_version", firmware_ota_current_version());
    char *response = cJSON_PrintUnformatted(root);
    if (response) {
        bluetooth_send_message(response);
        cJSON_free(response);
    } else {
        bluetooth_send_message("{\"ok\":false,\"error\":\"json_print\"}");
    }
    cJSON_Delete(root);
    vTaskDelay(pdMS_TO_TICKS(40));

    root = cJSON_CreateObject();
    cJSON *wifi = cJSON_CreateObject();
    if (!root || !wifi) {
        cJSON_Delete(root);
        cJSON_Delete(wifi);
        bluetooth_send_message("{\"ok\":false,\"error\":\"no_mem\"}");
        return;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "type", "ConfigWifi");
    cJSON_AddStringToObject(wifi, "SSID", wifi_ssid);
    cJSON_AddStringToObject(wifi, "password", wifi_password);
    cJSON_AddItemToObject(root, "wifi", wifi);
    response = cJSON_PrintUnformatted(root);
    if (response) {
        bluetooth_send_message(response);
        cJSON_free(response);
    } else {
        bluetooth_send_message("{\"ok\":false,\"error\":\"json_print\"}");
    }
    cJSON_Delete(root);
    vTaskDelay(pdMS_TO_TICKS(40));

    root = cJSON_CreateObject();
    cJSON *gateway = cJSON_CreateObject();
    if (!root || !gateway) {
        cJSON_Delete(root);
        cJSON_Delete(gateway);
        bluetooth_send_message("{\"ok\":false,\"error\":\"no_mem\"}");
        return;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "type", "ConfigGateway");
    cJSON_AddStringToObject(gateway, "UserGtw", userNameHTTPs);
    cJSON_AddStringToObject(gateway, "password", passwordHTTPs);
    cJSON_AddNumberToObject(gateway, "IdUser", ID_GATEWAY);
    cJSON_AddNumberToObject(gateway, "unidade", DEVICE_ID);
    cJSON_AddItemToObject(root, "gateway", gateway);
    response = cJSON_PrintUnformatted(root);
    if (response) {
        bluetooth_send_message(response);
        cJSON_free(response);
    } else {
        bluetooth_send_message("{\"ok\":false,\"error\":\"json_print\"}");
    }
    cJSON_Delete(root);
}

void bt_message_received_callback(const char *message) {
    printf("MSG Bluetooth Recebido \n");

    if (!message) {
        printf("Erro: Dados recebidos são nulos\n");
        return;
    }

    cJSON *jsonBluetooth = cJSON_Parse(message);

    if (jsonBluetooth == NULL) {
        printf("Erro ao parsear JSON\n");
        bluetooth_send_message("{\"ok\":false,\"error\":\"invalid_json\"}");
        return;
    }

    cJSON *JsonTypeCmd = cJSON_GetObjectItem(jsonBluetooth, "type");
    if (!cJSON_IsString(JsonTypeCmd)) {
        JsonTypeCmd = cJSON_GetObjectItem(jsonBluetooth, "cmd");
    }
    const char *cmd = cJSON_GetStringValue(JsonTypeCmd);

    printf("Comando recebido: %s\n", cmd ? cmd : "NULL");

    if (cmd && strcmp(cmd, "GetConfig") == 0) {
        bt_send_config_snapshot();
        cJSON_Delete(jsonBluetooth);
        return;
    }

    if (cmd && strcmp(cmd, "BleOtaBegin") == 0) {
        cJSON *size_item = cJSON_GetObjectItem(jsonBluetooth, "size");
        if (!cJSON_IsNumber(size_item) || size_item->valuedouble <= 0 ||
            size_item->valuedouble > (double)UINT32_MAX ||
            size_item->valuedouble != (double)(uint32_t)size_item->valuedouble) {
            bluetooth_send_message("{\"ok\":false,\"type\":\"BleOtaError\",\"error\":\"invalid_size\"}");
            cJSON_Delete(jsonBluetooth);
            return;
        }
        esp_err_t err = firmware_ota_ble_begin((uint32_t)size_item->valuedouble);
        if (err == ESP_OK) {
            bluetooth_send_message("{\"ok\":true,\"type\":\"BleOtaReady\",\"offset\":0}");
        } else {
            char response[112];
            snprintf(response, sizeof(response),
                     "{\"ok\":false,\"type\":\"BleOtaError\",\"error\":\"%s\"}", esp_err_to_name(err));
            bluetooth_send_message(response);
        }
        cJSON_Delete(jsonBluetooth);
        return;
    }

    if (cmd && strcmp(cmd, "BleOtaFinish") == 0) {
        char version[FW_VERSION_TEXT_MAX] = {0};
        esp_err_t err = firmware_ota_ble_finish(version, sizeof(version));
        if (err == ESP_OK) {
            char response[128];
            snprintf(response, sizeof(response),
                     "{\"ok\":true,\"type\":\"BleOtaComplete\",\"version\":\"%s\",\"restarting\":true}",
                     version);
            bluetooth_send_message(response);
            xTaskCreate(ble_ota_restart_task, "ble_ota_rst", 2048, NULL, 7, NULL);
        } else {
            char response[112];
            snprintf(response, sizeof(response),
                     "{\"ok\":false,\"type\":\"BleOtaError\",\"error\":\"%s\"}", esp_err_to_name(err));
            bluetooth_send_message(response);
        }
        cJSON_Delete(jsonBluetooth);
        return;
    }

    if (cmd && strcmp(cmd, "BleOtaCancel") == 0) {
        firmware_ota_ble_abort();
        bluetooth_send_message("{\"ok\":true,\"type\":\"BleOtaCancelled\"}");
        cJSON_Delete(jsonBluetooth);
        return;
    }

    if (cmd && strcmp(cmd, "GtwConfig") == 0) {
        cJSON *JsonUserGtw = cJSON_GetObjectItem(jsonBluetooth, "UserGtw");
        cJSON *JsonUserId = cJSON_GetObjectItem(jsonBluetooth, "IdUser");
        cJSON *JsonPassword = cJSON_GetObjectItem(jsonBluetooth, "password");
        cJSON *JsonUnidade = cJSON_GetObjectItem(jsonBluetooth, "unidade");

        // Verifica se todos os campos estão presentes e são válidos
        if (!cJSON_IsString(JsonUserGtw) || JsonUserGtw->valuestring == NULL) {
#if DEBUG_MODE
            printf("Erro: Campo 'UserGtw' ausente ou inválido\n");
#endif
            cJSON_Delete(jsonBluetooth);
            return;
        }

        if (!cJSON_IsString(JsonPassword) || JsonPassword->valuestring == NULL) {
#if DEBUG_MODE
            printf("Erro: Campo 'password' ausente ou inválido\n");
#endif
            cJSON_Delete(jsonBluetooth);
            return;
        }

        if (!cJSON_IsNumber(JsonUserId)) {
#if DEBUG_MODE
            printf("Erro: Campo 'IdUser' ausente ou inválido\n");
#endif
            cJSON_Delete(jsonBluetooth);
            return;
        }

        if (!cJSON_IsNumber(JsonUnidade)) {
#if DEBUG_MODE

            printf("Erro: Campo 'unidade' ausente ou inválido\n");
#endif
            cJSON_Delete(jsonBluetooth);
            return;
        }

        // Salva os valores no NVS
        save_UsetGTW(JsonUserGtw->valuestring);
        save_PasswordGTW(JsonPassword->valuestring);
        save_idGtw(JsonUserId->valueint);
        save_idUnidadeGtw(JsonUnidade->valueint);
        strlcpy(userNameHTTPs, JsonUserGtw->valuestring, sizeof(userNameHTTPs));
        strlcpy(passwordHTTPs, JsonPassword->valuestring, sizeof(passwordHTTPs));
        ID_GATEWAY = JsonUserId->valueint;
        DEVICE_ID = JsonUnidade->valueint;
        bluetooth_send_message("{\"ok\":true,\"status\":\"gtw_config_saved\",\"restart_required\":true}");

        cJSON_Delete(jsonBluetooth); // Libera a memória alocada para o JSON
        return;
    }

    if (cmd && strcmp(cmd, "GtwWifi") == 0) {
        cJSON *JsonSSIDWifi = cJSON_GetObjectItem(jsonBluetooth, "SSID");
        cJSON *JsonPasswordWifi = cJSON_GetObjectItem(jsonBluetooth, "password");

        // Verifica se todos os campos estão presentes e são válidos
        if (!cJSON_IsString(JsonSSIDWifi) || JsonSSIDWifi->valuestring == NULL) {
#if DEBUG_MODE
            printf("Erro: Campo 'ssidValue' ausente ou inválido\n");
#endif
            cJSON_Delete(jsonBluetooth);
            return;
        }
        if (!cJSON_IsString(JsonPasswordWifi) || JsonPasswordWifi->valuestring == NULL) {
#if DEBUG_MODE
            printf("Erro: Campo 'JsonPasswordWifi' ausente ou inválido\n");
#endif
            cJSON_Delete(jsonBluetooth);
            return;
        }

        save_SIIDWifi(JsonSSIDWifi->valuestring);
        save_PasswordWifi(JsonPasswordWifi->valuestring);
        bluetooth_send_message("{\"ok\":true,\"status\":\"wifi_config_saved\",\"restart_required\":true}");

        strlcpy(wifi_ssid, JsonSSIDWifi->valuestring, sizeof(wifi_ssid));
        strlcpy(wifi_password, JsonPasswordWifi->valuestring, sizeof(wifi_password));

        cJSON_Delete(jsonBluetooth);
        return;
    }

    if ((cmd && strcmp(cmd, "Reset") == 0)) {
        bluetooth_send_message("{\"ok\":true,\"status\":\"reset\"}");
        cJSON_Delete(jsonBluetooth);
        ConnectRest(); // Resetar connect
        return;
    }

    bluetooth_send_message("{\"ok\":false,\"error\":\"unknown_command\"}");
    cJSON_Delete(jsonBluetooth);
}

// Task para monitorar memoria
void vTaskImprimirUsoMemoria(void *pvParameters) {
    esp_task_wdt_add(NULL);

    static uint8_t boot_cycles = 0;

    while (1) {
        esp_task_wdt_reset();

        // Captura heap com API oficial
        multi_heap_info_t info;
        heap_caps_get_info(&info, MALLOC_CAP_8BIT);

        size_t heap_livre = info.total_free_bytes;
        size_t heap_total = info.total_allocated_bytes + heap_livre;
        float percentual_uso = ((float)(heap_total - heap_livre) / heap_total) * 100.0f;

#if DEBUG_MODE
        ESP_LOGI("MEMORY", "Heap total: %u | Livre: %u | Uso: %.2f%%", (unsigned)heap_total, (unsigned)heap_livre,
                 percentual_uso);
#endif

        // Low memory check
        if (percentual_uso > 94.0f) {
            if (bluetooth_config_is_active()) {
                ESP_LOGW("MEMORY", "Heap critico com BLE ativo (%.2f%%). Fechando BLE antes de resetar.",
                         percentual_uso);
                bluetooth_config_stop();
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            ESP_LOGW("MEMORY", "Heap crítico (%.2f%%). Sinalizando reset do stack de rede...", percentual_uso);
            ConnectRest();
        }

        // Recriar websocket se morreu (somente uma vez)
#if (GTW_ROLE_RX_ONLY == 0)
        if (boot_cycles > 3) {
            if (taskConnect_to_websocket == NULL && TokenOk && Connectado) {
                ESP_LOGW("MEMORY", "Reconectando WebSocket...");
                xTaskCreate(connect_to_websocket, "connect_to_websocket", 2048 * 6, NULL, 5, &taskConnect_to_websocket);
            }
        } else {
            boot_cycles++;
        }
#else
        boot_cycles++;
#endif

        vTaskDelay(pdMS_TO_TICKS(15000 + esp_random() % 2000)); // jitter leve
    }
}

/*#########################################  Funçoes de Configuraçao Bluetooth #########################################
 */

// // Função para salvar A SIID do wifi na NVS
void save_SIIDWifi(const char *device_name) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        nvs_set_str(my_handle, "SSID", device_name);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    } else {
        ESP_LOGE("NVS", "Erro ao abrir NVS handle!");
    }
}

void save_PasswordWifi(const char *password) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        // Armazenando a senha no NVS com a chave "passwordGtw"
        nvs_set_str(my_handle, "passwordWifi", password);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    } else {
        ESP_LOGE("NVS", "Erro ao abrir NVS handle para salvar a senha do WIfi!");
    }
}

// Função para salvar o estado na NVS
void save_UsetGTW(const char *device_name) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        nvs_set_str(my_handle, "userGtw", device_name);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    } else {
        ESP_LOGE("NVS", "Erro ao abrir NVS handle!");
    }
}

void save_PasswordGTW(const char *password) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        // Armazenando a senha no NVS com a chave "passwordGtw"
        nvs_set_str(my_handle, "passwordGtw", password);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    } else {
        ESP_LOGE("NVS", "Erro ao abrir NVS handle para salvar a senha!");
    }
}

// Função para salvar o estado ID do GTW na NVS
void save_idGtw(int state) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("idGtw", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE("NVS", "Falha ao abrir idGtw para escrita: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_i32(my_handle, "idGtw", state);
    if (err == ESP_OK) {
        err = nvs_commit(my_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE("NVS", "Falha ao salvar idGtw: %s", esp_err_to_name(err));
    }
    nvs_close(my_handle);
}

// Função para salvar o ID da Unidade do GTW na NVS
void save_idUnidadeGtw(int state) {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("idUnidadeGtw", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE("NVS", "Falha ao abrir idUnidadeGtw para escrita: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_i32(my_handle, "idUnidadeGtw", state);
    if (err == ESP_OK) {
        err = nvs_commit(my_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE("NVS", "Falha ao salvar idUnidadeGtw: %s", esp_err_to_name(err));
    }
    nvs_close(my_handle);
}

/*######################################### Tratamento de Erros ############################################*/

// Reinicia o sistema
void ConnectRest() {
    printf("\033[1;36m\n\n========== SISTEMA REINICIANDO: Connect ==========\n\n\033[0m");

    // Resetar GPIOs (já presente e recomendado)
    // gpio_reset_pin(I2C_SDA);
    // gpio_reset_pin(I2C_SCL);

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart(); // Reinicia o ESP
}
