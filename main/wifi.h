#pragma once

#include "esp_err.h"
#include <stdbool.h>


#define WIFI_SECONDARY_SSID "ACLS4G_GTW" // "" se não tiver
#define WIFI_SECONDARY_PASS "Acls@1234"

#define WIFI_CONNECT_TIMEOUT_MS 8000
#define INTERNET_CHECK_TIMEOUT_MS 3000

bool wifi_start_driver(void);
void wifi_stop_driver(void);
bool wifi_connect_credentials(const char *ssid, const char *pass, int timeout_ms);
bool wifi_is_active(void);
bool wifi_sta_connected(void);
bool wifi_check_internet(int timeout_ms);