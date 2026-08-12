#include "wifi.h"
#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include <errno.h>
#include <string.h>

#ifdef TAG
#undef TAG
#endif

static const char *TAG = "WiFiConnect";
static EventGroupHandle_t wifi_event_group;

static const int WIFI_IPV4_READY_BIT = BIT0;
static const int WIFI_FAIL_BIT = BIT1;
static const int WIFI_IPV6_READY_BIT = BIT2;

#if GTW_USE_IPV6
#define WIFI_SELECTED_IP_READY_BIT WIFI_IPV6_READY_BIT
#define GTW_SOCKET_ADDR_FAMILY AF_INET6
#else
#define WIFI_SELECTED_IP_READY_BIT WIFI_IPV4_READY_BIT
#define GTW_SOCKET_ADDR_FAMILY AF_INET
#endif

static bool netif_inited = false;
static bool wifi_started = false;
static bool handlers_registered = false;
static esp_netif_t *sta_netif = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_event_group, WIFI_IPV4_READY_BIT | WIFI_IPV6_READY_BIT);
        xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        ESP_LOGW(TAG, "Wi-Fi desconectado");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
#if GTW_USE_IPV6
        esp_err_t err = esp_netif_create_ip6_linklocal(sta_netif);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Falha ao iniciar IPv6 na interface Wi-Fi: %s", esp_err_to_name(err));
        }
#endif
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_IPV4_READY_BIT);
        ESP_LOGI(TAG, "Wi-Fi obteve IPv4");
    } else if (base == IP_EVENT && id == IP_EVENT_GOT_IP6) {
        ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)data;
        esp_ip6_addr_type_t type = esp_netif_ip6_get_addr_type(&event->ip6_info.ip);
        ESP_LOGI(TAG, "Wi-Fi obteve IPv6: " IPV6STR " (tipo=%d)", IPV62STR(event->ip6_info.ip), type);
        if (type == ESP_IP6_ADDR_IS_GLOBAL || type == ESP_IP6_ADDR_IS_UNIQUE_LOCAL) {
            xEventGroupSetBits(wifi_event_group, WIFI_IPV6_READY_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(wifi_event_group, WIFI_IPV4_READY_BIT);
        ESP_LOGW(TAG, "Wi-Fi perdeu o endereco IPv4");
    }
}

bool wifi_start_driver(void) {
    if (wifi_started)
        return true;

    if (!netif_inited) {
        esp_err_t init_err = esp_netif_init();
        if (init_err != ESP_OK && init_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Falha ao inicializar esp_netif: %s", esp_err_to_name(init_err));
            return false;
        }

        init_err = esp_event_loop_create_default();
        if (init_err != ESP_OK && init_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Falha ao criar event loop: %s", esp_err_to_name(init_err));
            return false;
        }
        netif_inited = true;
    }

    if (!sta_netif) {
        sta_netif = esp_netif_create_default_wifi_sta();
        if (!sta_netif) {
            ESP_LOGE(TAG, "Falha ao criar interface Wi-Fi STA");
            return false;
        }
    }

    if (!wifi_event_group) {
        wifi_event_group = xEventGroupCreate();
        if (!wifi_event_group) {
            ESP_LOGE(TAG, "Falha ao criar grupo de eventos Wi-Fi");
            return false;
        }
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha em esp_wifi_init: %s", esp_err_to_name(err));
        return false;
    }
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ||
        esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) {
        esp_wifi_deinit();
        return false;
    }

    if (!handlers_registered) {
        err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao registrar eventos Wi-Fi: %s", esp_err_to_name(err));
            esp_wifi_deinit();
            return false;
        }

        err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao registrar eventos IP: %s", esp_err_to_name(err));
            esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
            esp_wifi_deinit();
            return false;
        }
        handlers_registered = true;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha em esp_wifi_start: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        return false;
    }
    wifi_started = true;
    ESP_LOGI(TAG, "Driver Wi-Fi iniciado; conexoes configuradas para %s", GTW_IP_VERSION_NAME);
    return true;
}

void wifi_stop_driver(void) {
    if (wifi_started) {
        ESP_LOGW(TAG, "Desligando driver Wi-Fi...");
        esp_wifi_disconnect();
        esp_wifi_stop();
        esp_wifi_deinit();
        wifi_started = false;
        xEventGroupClearBits(wifi_event_group, WIFI_IPV4_READY_BIT | WIFI_IPV6_READY_BIT | WIFI_FAIL_BIT);
    }
}

bool wifi_connect_credentials(const char *ssid, const char *pass, int timeout_ms) {
    if (!wifi_started || !wifi_event_group || !ssid || ssid[0] == '\0') {
        ESP_LOGE(TAG, "Tentativa de conexao Wi-Fi com driver ou credenciais invalidas");
        return false;
    }

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);

    xEventGroupClearBits(wifi_event_group, WIFI_IPV4_READY_BIT | WIFI_IPV6_READY_BIT | WIFI_FAIL_BIT);

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar Wi-Fi: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Falha ao iniciar conexao Wi-Fi: %s", esp_err_to_name(err));
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_SELECTED_IP_READY_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));

    return (bits & WIFI_SELECTED_IP_READY_BIT);
}

bool wifi_is_active(void) { return wifi_started; }

bool wifi_sta_connected(void) {
    wifi_ap_record_t ap;
    return (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
}

bool wifi_sta_has_ip(void) {
    if (!wifi_started || !sta_netif || !wifi_event_group) {
        return false;
    }

#if GTW_USE_IPV6
    esp_ip6_addr_t addresses[CONFIG_LWIP_IPV6_NUM_ADDRESSES] = {0};
    int address_count = esp_netif_get_all_preferred_ip6(sta_netif, addresses);

    for (int i = 0; i < address_count; i++) {
        esp_ip6_addr_type_t type = esp_netif_ip6_get_addr_type(&addresses[i]);
        if (type == ESP_IP6_ADDR_IS_GLOBAL || type == ESP_IP6_ADDR_IS_UNIQUE_LOCAL) {
            xEventGroupSetBits(wifi_event_group, WIFI_IPV6_READY_BIT);
            return true;
        }
    }

    xEventGroupClearBits(wifi_event_group, WIFI_IPV6_READY_BIT);
    return false;
#else
    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        xEventGroupSetBits(wifi_event_group, WIFI_IPV4_READY_BIT);
        return true;
    }

    xEventGroupClearBits(wifi_event_group, WIFI_IPV4_READY_BIT);
    return false;
#endif
}

bool wifi_check_internet(int timeout_ms) {
    struct addrinfo hints = {0};
    struct addrinfo *res = NULL;
    hints.ai_family = GTW_SOCKET_ADDR_FAMILY;
    hints.ai_socktype = SOCK_STREAM;

    int dns_err = getaddrinfo("www.google.com", "443", &hints, &res);
    if (dns_err != 0 || !res) {
        ESP_LOGW(TAG, "Falha no teste DNS da internet: erro=%d", dns_err);
        return false;
    }

    bool ok = false;
    int last_errno = 0;
    struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};

    for (struct addrinfo *addr = res; addr != NULL; addr = addr->ai_next) {
        int sock = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (sock < 0) {
            last_errno = errno;
            continue;
        }

        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(sock, addr->ai_addr, addr->ai_addrlen) == 0) {
            ok = true;
            close(sock);
            break;
        }

        last_errno = errno;
        close(sock);
    }

    freeaddrinfo(res);

    if (!ok)
        ESP_LOGW(TAG, "Teste TCP da internet falhou: errno=%d", last_errno);

    return ok;
}
