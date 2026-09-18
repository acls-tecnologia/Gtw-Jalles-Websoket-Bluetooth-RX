#include "config.h"

/********** CONTADORES **********/
uint16_t msg_counter_gtw = 1;

/********** WEBSOCKET **********/
#if (GTW_ROLE_RX_ONLY == 0)
char *websocket_url = NULL;
esp_websocket_client_handle_t ws_client = NULL;
#endif
uint8_t Contador_Erro_HTTP = 0;

/********** FILAS **********/
#if (GTW_ROLE_TX_ONLY == 0)
QueueHandle_t xPatchQueue = NULL;
QueueHandle_t xPatchFreeQueue = NULL;
#endif
#if (GTW_ROLE_RX_ONLY == 0)
QueueHandle_t ws_msg_queue = NULL;
#endif

/********** SEMÁFOROS **********/
SemaphoreHandle_t MutexHTTP = NULL;
SemaphoreHandle_t MutexLora = NULL;
SemaphoreHandle_t i2c_semaphore = NULL;
SemaphoreHandle_t poco_mutex = NULL;

/********** TIMERS **********/
esp_timer_handle_t watchdog_timer = NULL;
esp_timer_handle_t token_timer = NULL;

/********** TASKS **********/
TaskHandle_t taskConecta_WIFI = NULL;
TaskHandle_t taskImprimirUsoMemoria = NULL;
TaskHandle_t taskGet_Nivel = NULL;
TaskHandle_t taskGet_Corrente = NULL;
TaskHandle_t taskGet_Tensoes = NULL;
TaskHandle_t taskVerifica_Token = NULL;
#if (GTW_ROLE_RX_ONLY == 0)
TaskHandle_t taskConnect_to_websocket = NULL;
#endif
#if (GTW_ROLE_TX_ONLY == 0)
TaskHandle_t task_patch = NULL;
#endif
TaskHandle_t TaskLerSensores = NULL;
TaskHandle_t Task_login_task = NULL;
TaskHandle_t controllerTaskHandle = NULL;

/********** VARIÁVEIS **********/
int InternetInit_Cont = 0;
uint8_t Forca_update = 0;
int NVS_Recuperado = 0;
int wifi_Reniciar = 0;

int ID_GATEWAY = 0;
int DEVICE_ID = 0;

int bleOFF = 0;

char userNameHTTPs[64] = {0};
char passwordHTTPs[65] = {0};

bool wifi_secundario = false;
bool usando_secundario = false;
bool Modem = false;
bool PressaoBomba = false;

int wifi_secundario_ativo = 0;

char wifi_ssid[33] = {0};
char wifi_password[65] = {0};

#if (GTW_ROLE_RX_ONLY == 0)
const char *TAG_Websocket = "WEBSOCKET_CLIENT";
volatile bool stop_websocket_task = false;
#endif

int TokenOk = 0;

size_t heap_total = 0;

/********** CERTIFICADO **********/
const char *rootCaCerticate =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n"
    "TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n"
    "cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\n"
    "WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu\n"
    "ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY\n"
    "MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc\n"
    "h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+\n"
    "0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U\n"
    "A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW\n"
    "T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH\n"
    "B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC\n"
    "B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv\n"
    "KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn\n"
    "OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn\n"
    "jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw\n"
    "qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI\n"
    "rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV\n"
    "HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq\n"
    "hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\n"
    "ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ\n"
    "3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK\n"
    "NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5\n"
    "ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur\n"
    "TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC\n"
    "jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc\n"
    "oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq\n"
    "4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA\n"
    "mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d\n"
    "emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=\n"
    "-----END CERTIFICATE-----\n";
