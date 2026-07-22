#include "bluetooth.h"
#include "main.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define BLE_TAG "GTW_BLE"
#define BLE_DEVICE_NAME "GTW-Jalles-BLE"
#define BLE_DEFAULT_WINDOW_MS (60 * 1000)
#define BLE_RX_MAX_LEN 512
#define BLE_QUEUE_LEN 4

#define GTW_BLE_SERVICE_UUID 0xFFF0
#define GTW_BLE_RX_UUID 0xFFF1
#define GTW_BLE_TX_UUID 0xFFF2

typedef struct {
    char data[BLE_RX_MAX_LEN + 1];
} ble_rx_msg_t;

static uint8_t own_addr_type;
static uint16_t tx_val_handle;
static uint16_t active_conn_handle = BLE_HS_CONN_HANDLE_NONE;

static bool ble_stack_started = false;
static bool ble_stack_stopping = false;
static bool ble_window_open = false;
static bool notify_enabled = false;

static char last_status[512] = "{\"ok\":true,\"status\":\"idle\"}";

static QueueHandle_t ble_rx_queue = NULL;
static TaskHandle_t ble_rx_task_handle = NULL;
static TimerHandle_t ble_window_timer = NULL;

static void ble_advertise(void);
static void ble_advertise_async(void);
static void ble_stop_async(void);

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(GTW_BLE_SERVICE_UUID),
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = BLE_UUID16_DECLARE(GTW_BLE_RX_UUID),
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = BLE_UUID16_DECLARE(GTW_BLE_TX_UUID),
                .access_cb = gatt_access_cb,
                .val_handle = &tx_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {0},
        },
    },
    {0},
};

static void ble_rx_task(void *arg)
{
    ble_rx_msg_t msg;

    while (1) {
        if (xQueueReceive(ble_rx_queue, &msg, portMAX_DELAY) == pdTRUE) {
            bt_message_received_callback(msg.data);
        }
    }
}

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_advertise_task(void *arg)
{
    if (ble_window_open) {
        ble_advertise();
    }
    vTaskDelete(NULL);
}

static void ble_advertise_async(void)
{
    xTaskCreate(ble_advertise_task, "ble_adv", 3072, NULL, 5, NULL);
}

static void set_status(const char *status)
{
    snprintf(last_status, sizeof(last_status), "%s", status ? status : "{\"ok\":false}");
}

static void notify_status(void)
{
    if (!ble_window_open || active_conn_handle == BLE_HS_CONN_HANDLE_NONE || !notify_enabled) {
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(last_status, strlen(last_status));
    if (!om) {
        return;
    }

    int rc = ble_gattc_notify_custom(active_conn_handle, tx_val_handle, om);
    if (rc != 0) {
        ESP_LOGW(BLE_TAG, "Falha ao notificar status BLE: %d", rc);
    }
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            active_conn_handle = event->connect.conn_handle;
            if (ble_window_timer) {
                xTimerStop(ble_window_timer, 0);
            }
            bt_client_connected_callback();
            ESP_LOGI(BLE_TAG, "Cliente BLE conectado");
        } else {
            active_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            notify_enabled = false;
            if (ble_window_open) {
                ble_advertise_async();
            }
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(BLE_TAG, "Cliente BLE desconectado");
        active_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        notify_enabled = false;
        bt_client_disconnected_callback();
        if (ble_window_open) {
            ble_stop_async();
        }
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (ble_window_open) {
            ble_advertise_async();
        }
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == tx_val_handle) {
            notify_enabled = event->subscribe.cur_notify;
            notify_status();
        }
        break;

    default:
        break;
    }

    return 0;
}

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, last_status, strlen(last_status)) == 0
                   ? 0
                   : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 0 || len > BLE_RX_MAX_LEN) {
            set_status("{\"ok\":false,\"error\":\"payload_size\"}");
            notify_status();
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        ble_rx_msg_t msg = {0};
        int rc = ble_hs_mbuf_to_flat(ctxt->om, msg.data, BLE_RX_MAX_LEN, &len);
        if (rc != 0) {
            set_status("{\"ok\":false,\"error\":\"payload_copy\"}");
            notify_status();
            return BLE_ATT_ERR_UNLIKELY;
        }
        msg.data[len] = '\0';

        if (xQueueSend(ble_rx_queue, &msg, 0) != pdTRUE) {
            set_status("{\"ok\":false,\"error\":\"queue_full\"}");
            notify_status();
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }

        set_status("{\"ok\":true,\"status\":\"received\"}");
        notify_status();
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static void ble_advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    struct ble_hs_adv_fields rsp_fields = {0};
    struct ble_gap_adv_params adv_params = {0};
    ble_uuid16_t service_uuid = BLE_UUID16_INIT(GTW_BLE_SERVICE_UUID);

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids16 = &service_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    rsp_fields.name = (const uint8_t *)BLE_DEVICE_NAME;
    rsp_fields.name_len = strlen(BLE_DEVICE_NAME);
    rsp_fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "Falha ao configurar advertising BLE: %d", rc);
        return;
    }

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "Falha ao configurar scan response BLE: %d", rc);
        return;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(BLE_TAG, "Falha ao iniciar advertising BLE: %d", rc);
    }
}

static void ble_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(BLE_TAG, "Falha ao inferir endereco BLE: %d", rc);
        return;
    }

    if (ble_window_open) {
        ble_advertise();
    }
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(BLE_TAG, "Reset do host BLE: %d", reason);
}

static void ble_stop_task(void *arg)
{
    bluetooth_config_stop();
    vTaskDelete(NULL);
}

static void ble_stop_async(void)
{
    xTaskCreate(ble_stop_task, "ble_stop", 3072, NULL, 5, NULL);
}

static void ble_window_timeout_cb(TimerHandle_t timer)
{
    if (active_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGI(BLE_TAG, "Timeout BLE ignorado: cliente conectado");
        return;
    }

    ESP_LOGI(BLE_TAG, "Nenhum cliente BLE conectado no tempo limite; fechando BLE");
    ble_stop_async();
}

static esp_err_t ble_prepare_runtime(void)
{
    if (!ble_rx_queue) {
        ble_rx_queue = xQueueCreate(BLE_QUEUE_LEN, sizeof(ble_rx_msg_t));
        if (!ble_rx_queue) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!ble_rx_task_handle) {
        BaseType_t ok = xTaskCreate(ble_rx_task, "ble_rx", 4096, NULL, 5, &ble_rx_task_handle);
        if (ok != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!ble_window_timer) {
        ble_window_timer = xTimerCreate("ble_window", pdMS_TO_TICKS(BLE_DEFAULT_WINDOW_MS),
                                        pdFALSE, NULL, ble_window_timeout_cb);
        if (!ble_window_timer) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

esp_err_t bluetooth_config_start(uint32_t timeout_ms)
{
    esp_err_t err = ble_prepare_runtime();
    if (err != ESP_OK) {
        ESP_LOGE(BLE_TAG, "Falha ao preparar BLE: %s", esp_err_to_name(err));
        return err;
    }

    if (timeout_ms == BLUETOOTH_CONFIG_TIMEOUT_DEFAULT_MS) {
        timeout_ms = BLE_DEFAULT_WINDOW_MS;
    }

    ble_window_open = true;
    set_status("{\"ok\":true,\"status\":\"advertising\"}");

    if (!ble_stack_started) {
        err = nimble_port_init();
        if (err != ESP_OK) {
            ble_window_open = false;
            ESP_LOGE(BLE_TAG, "nimble_port_init falhou: %s", esp_err_to_name(err));
            return err;
        }

        ble_hs_cfg.reset_cb = ble_on_reset;
        ble_hs_cfg.sync_cb = ble_on_sync;

        ble_svc_gap_init();
        ble_svc_gatt_init();
        ble_svc_gap_device_name_set(BLE_DEVICE_NAME);

        int rc = ble_gatts_count_cfg(gatt_services);
        if (rc == 0) {
            rc = ble_gatts_add_svcs(gatt_services);
        }
        if (rc != 0) {
            ble_window_open = false;
            ESP_LOGE(BLE_TAG, "Falha ao registrar GATT BLE: %d", rc);
            return ESP_FAIL;
        }

        nimble_port_freertos_init(ble_host_task);
        ble_stack_started = true;
    } else {
        ble_advertise();
    }

    if (timeout_ms == BLUETOOTH_CONFIG_TIMEOUT_FOREVER_MS) {
        xTimerStop(ble_window_timer, 0);
        ESP_LOGI(BLE_TAG, "Janela BLE aberta sem timeout");
    } else {
        xTimerChangePeriod(ble_window_timer, pdMS_TO_TICKS(timeout_ms), 0);
        xTimerStart(ble_window_timer, 0);
        ESP_LOGI(BLE_TAG, "Janela BLE aberta por %lu ms", (unsigned long)timeout_ms);
    }

    return ESP_OK;
}

void bluetooth_config_stop(void)
{
    if (!ble_window_open && !ble_stack_started) {
        return;
    }

    ble_window_open = false;
    notify_enabled = false;

    if (ble_window_timer) {
        xTimerStop(ble_window_timer, 0);
    }

    if (active_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(active_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        active_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ble_gap_adv_stop();

    if (ble_stack_started && !ble_stack_stopping) {
        ble_stack_stopping = true;

        int stop_rc = nimble_port_stop();
        if (stop_rc != 0) {
            ESP_LOGW(BLE_TAG, "nimble_port_stop retornou %d", stop_rc);
        }

        esp_err_t deinit_err = ESP_FAIL;
        for (int i = 0; i < 10; i++) {
            deinit_err = nimble_port_deinit();
            if (deinit_err == ESP_OK) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        if (deinit_err == ESP_OK) {
            ble_stack_started = false;
            tx_val_handle = 0;
            ESP_LOGI(BLE_TAG, "Stack BLE/NimBLE desligada e memoria liberada");
        } else {
            ESP_LOGW(BLE_TAG, "Falha ao deinit NimBLE: %s", esp_err_to_name(deinit_err));
        }

        ble_stack_stopping = false;
    }

    set_status("{\"ok\":true,\"status\":\"closed\"}");
    ESP_LOGI(BLE_TAG, "Janela BLE fechada");
}

bool bluetooth_config_is_active(void)
{
    return ble_window_open;
}

void bluetooth_send_message(const char *message)
{
    if (!message) {
        return;
    }

    set_status(message);
    notify_status();
}

void bluetooth_init(void)
{
    bluetooth_config_start(BLE_DEFAULT_WINDOW_MS);
}
