#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h> // Para bool, true, false
#include <stddef.h>  // Para size_t
#include <stdint.h>  // Para uint8_t, int8_t
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <math.h>
#include <stdio.h>

#include "driver/ledc.h"

#include "esp_heap_trace.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

#include "bluetooth.h"
#include "esp_heap_caps.h"
#include "http_request.h"
#include "lora.h"
#include "salvar_nvs.h"
#include "wifi.h"

/***********************************************
 * CONSTANTES GERAIS
 ***********************************************/
#define GTW_ROLE_TX_ONLY 0
#define GTW_ROLE_RX_ONLY 1
#define GTW_ROLE_NAME "GTW_RX"

#if (GTW_ROLE_TX_ONLY == GTW_ROLE_RX_ONLY)
#error "Configure exatamente um papel: GTW_ROLE_TX_ONLY ou GTW_ROLE_RX_ONLY"
#endif

#define ACK_TIMEOUT_MS 15000
#define ACK_RETRIES 3
#define LORA_TRANSACTION_GUARD_MS 1500
#define LORA_RETRY_BACKOFF_MIN_MS 2500
#define LORA_RETRY_BACKOFF_JITTER_MS 7000
#define GTW_LORA_RX_TIMEOUT_MS 800
#define GTW_TX_IDLE_RX_TIMEOUT_MS 40
#define GTW_TX_IDLE_RX_SLEEP_MS 120
#define GTW_LORA_MUTEX_WAIT_MS 300

extern int DEVICE_ID;

#define DEBUG_MODE 1

#define Get_Estacao_Rota "/estacao/leitura/"
#define Firm_Leitura_id "/files/leitura/"
#define Firm_Download_id "/files/download/"
#define leituraGtw "/users/gtw/"

#define MAX_RETRIES_CONFIG 10
#define TAG "Main"

#define TAG_atualizacao_OTA "Atialzação OTA"
#define TAG_HTTP_Firm_Version "HTTP_Firm_Version"
#define TAG_Atualiza_firmware "Atualiza_firmware"
#define NVS_KEY_Fversion "Firm_version"

#define MAX_RECONNECT_ATTEMPTS 8
#define PATH_SIZE 64
#define BODY_SIZE 256
#define QUEUE_LENGTH 64
#define WS_MSG_MAX_LEN 512
#define WS_QUEUE_LEN 20

#define WIFI_INTERNET_MAX_FAILS 5
#define WIFI_CONNECT_MAX_FAILS 5
#define WIFI_CYCLE_MAX_RETRIES 5
#define WIFI_OFF_DELAY_MS 10000

#define LORA_PREAMBLE 0xAA

/***********************************************
 * ENUMS
 ***********************************************/
typedef enum { DEV_GTW = 0x01, DEV_TANK = 0x02, DEV_REP = 0x03 } device_type_t;

typedef enum { MSG_DATA = 0x01, MSG_CMD = 0x02, MSG_ACK = 0x03, MSG_PING = 0x04 } msg_type_t;

/***********************************************
 * ESTRUTURAS
 ***********************************************/
typedef struct {
    char path[PATH_SIZE];
    char body[BODY_SIZE];
    http_method_t method;
} PatchRequest;

typedef struct {
    char *data;
    int len;
} ws_msg_item_t;

typedef struct __attribute__((packed)) {
    uint8_t preamble;
    uint8_t src_type;
    uint16_t src_id;
    uint8_t dst_type;
    uint16_t dst_id;
    uint8_t msg_type;
    uint8_t msg_id;
    uint8_t len;

    uint8_t has_bomba_id;
    uint16_t bomba_id;

    uint8_t payload[64];
    uint16_t crc;
} lora_app_frame_t;

_Static_assert(sizeof(lora_app_frame_t) == 79, "Formato LoRa GTW incompatível");

/***********************************************
 * VARIÁVEIS GLOBAIS (extern)
 ***********************************************/
extern uint16_t msg_counter_gtw;

#if (GTW_ROLE_RX_ONLY == 0)
extern char *websocket_url;
#endif

extern uint8_t Contador_Erro_HTTP;

#if (GTW_ROLE_RX_ONLY == 0)
extern esp_websocket_client_handle_t ws_client;
#endif

/********** FILAS **********/
#if (GTW_ROLE_TX_ONLY == 0)
extern QueueHandle_t xPatchQueue;
extern QueueHandle_t xPatchFreeQueue;
#endif
#if (GTW_ROLE_RX_ONLY == 0)
extern QueueHandle_t ws_msg_queue;
#endif

/********** SEMÁFOROS **********/
extern SemaphoreHandle_t MutexHTTP;
extern SemaphoreHandle_t MutexLora;
extern SemaphoreHandle_t i2c_semaphore;
extern SemaphoreHandle_t poco_mutex;

/********** TIMERS **********/
extern esp_timer_handle_t watchdog_timer;
extern esp_timer_handle_t token_timer;

/********** TASK HANDLES **********/
extern TaskHandle_t taskConecta_WIFI;
extern TaskHandle_t taskImprimirUsoMemoria;
extern TaskHandle_t taskGet_Nivel;
extern TaskHandle_t taskGet_Corrente;
extern TaskHandle_t taskGet_Tensoes;
extern TaskHandle_t taskVerifica_Token;
#if (GTW_ROLE_RX_ONLY == 0)
extern TaskHandle_t taskConnect_to_websocket;
#endif
#if (GTW_ROLE_TX_ONLY == 0)
extern TaskHandle_t task_patch;
#endif
extern TaskHandle_t TaskLerSensores;
extern TaskHandle_t Task_login_task;
extern TaskHandle_t controllerTaskHandle;

/********** VARIÁVEIS GLOBAIS **********/
extern float Firmware_version;
extern int InternetInit_Cont;
extern uint8_t Forca_update;
extern int NVS_Recuperado;
extern int wifi_Reniciar;

extern int DEVICE_ID;
extern int ID_GATEWAY;

extern int bleOFF;

extern char userNameHTTPs[30];
extern char passwordHTTPs[25];

extern bool wifi_secundario;
extern bool usando_secundario;
extern bool Modem;
extern bool PressaoBomba;

extern int wifi_secundario_ativo;

extern char wifi_ssid[30];
extern char wifi_password[25];

extern int TokenOk;

#if (GTW_ROLE_RX_ONLY == 0)
extern const char *TAG_Websocket;
extern volatile bool stop_websocket_task;
#endif

extern size_t heap_total;

/********** CERTIFICADO **********/
extern const char *rootCaCerticate;

#endif // CONFIG_H
