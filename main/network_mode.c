#include "network_mode.h"

#include "config.h"
#include "esp_tls.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include <string.h>

#define GTW_WEBSOCKET_TLS_PORT 443

esp_err_t gtw_websocket_transport_init(gtw_websocket_transport_t *transport, const char *path,
                                       const char *cert_pem) {
    if (!transport || !path || !cert_pem) {
        return ESP_ERR_INVALID_ARG;
    }

    *transport = (gtw_websocket_transport_t){0};
    transport->tls = esp_transport_ssl_init();
    if (!transport->tls) {
        return ESP_ERR_NO_MEM;
    }

#if GTW_USE_IPV6
    esp_transport_ssl_set_addr_family(transport->tls, ESP_TLS_AF_INET6);
#else
    esp_transport_ssl_set_addr_family(transport->tls, ESP_TLS_AF_INET);
#endif
    esp_transport_ssl_set_cert_data(transport->tls, cert_pem, strlen(cert_pem));
    esp_transport_set_default_port(transport->tls, GTW_WEBSOCKET_TLS_PORT);

    transport->websocket = esp_transport_ws_init(transport->tls);
    if (!transport->websocket) {
        gtw_websocket_transport_cleanup(transport);
        return ESP_ERR_NO_MEM;
    }

    const esp_transport_ws_config_t ws_config = {
        .ws_path = path,
        .propagate_control_frames = true,
    };
    esp_err_t err = esp_transport_ws_set_config(transport->websocket, &ws_config);
    if (err != ESP_OK) {
        gtw_websocket_transport_cleanup(transport);
        return err;
    }

    esp_transport_set_default_port(transport->websocket, GTW_WEBSOCKET_TLS_PORT);
    return ESP_OK;
}

void gtw_websocket_transport_cleanup(gtw_websocket_transport_t *transport) {
    if (!transport) {
        return;
    }
    if (transport->websocket) {
        esp_transport_destroy(transport->websocket);
        transport->websocket = NULL;
    }
    if (transport->tls) {
        esp_transport_destroy(transport->tls);
        transport->tls = NULL;
    }
}
