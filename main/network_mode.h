#pragma once

#include "esp_err.h"
#include "esp_transport.h"

typedef struct {
    esp_transport_handle_t tls;
    esp_transport_handle_t websocket;
} gtw_websocket_transport_t;

esp_err_t gtw_websocket_transport_init(gtw_websocket_transport_t *transport, const char *path,
                                       const char *cert_pem);
void gtw_websocket_transport_cleanup(gtw_websocket_transport_t *transport);
