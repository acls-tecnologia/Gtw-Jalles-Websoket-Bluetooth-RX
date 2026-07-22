#include "wifi.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include <string.h>

static const char *TAG = "WiFiConnect";
static EventGroupHandle_t wifi_event_group;

static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT = BIT1;

static bool netif_inited = false;
static bool wifi_started = false;
static bool handlers_registered = false;
static esp_netif_t *sta_netif = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        ESP_LOGW(TAG, "Wi-Fi desconectado");
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi obteve IP");
    }
}

bool wifi_start_driver(void)
{
    if (wifi_started)
        return true;

    if (!netif_inited)
    {
        esp_netif_init();
        esp_event_loop_create_default();
        netif_inited = true;
    }

    if (!sta_netif)
        sta_netif = esp_netif_create_default_wifi_sta();

    if (!wifi_event_group)
        wifi_event_group = xEventGroupCreate();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK)
        return false;
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ||
        esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) {
        esp_wifi_deinit();
        return false;
    }

    if (!handlers_registered)
    {
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
        handlers_registered = true;
    }

    if (esp_wifi_start() != ESP_OK) {
        esp_wifi_deinit();
        return false;
    }
    wifi_started = true;
    ESP_LOGI(TAG, "Driver Wi-Fi iniciado");
    return true;
}

void wifi_stop_driver(void)
{
    if (wifi_started)
    {
        ESP_LOGW(TAG, "Desligando driver Wi-Fi...");
        esp_wifi_disconnect();
        esp_wifi_stop();
        esp_wifi_deinit();
        wifi_started = false;
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }
}

bool wifi_connect_credentials(const char *ssid, const char *pass, int timeout_ms)
{
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);

    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    return (bits & WIFI_CONNECTED_BIT);
}

bool wifi_is_active(void)
{
    return wifi_started;
}

bool wifi_sta_connected(void)
{
    wifi_ap_record_t ap;
    return (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
}

bool wifi_check_internet(int timeout_ms)
{
    struct addrinfo hints = {0};
    struct addrinfo *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo("83cd1aa837661fab941b2c5a2a65424b.jm.net.br", "2087", &hints, &res) != 0 || !res)
        return false;

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0)
    {
        freeaddrinfo(res);
        return false;
    }

    struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    bool ok = (connect(sock, res->ai_addr, res->ai_addrlen) == 0);
    close(sock);
    freeaddrinfo(res);
    return ok;
}
