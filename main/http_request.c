#include "http_request.h"
#include "cJSON.h"
#include "config.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "lwip/netdb.h"
#include <stdlib.h>
#include <string.h>

// TAG para logs
static const char *TAG_GET = "HTTP_GET_JSON";
static const char *TAGLogin = "LOGIN";

char token_global[700] = {0}; // Variável global para guardar o token
int Connectado = 0;
char full_url[256];

static char *response_buffer = NULL;
static int response_len = 0;

// Buffer global para receber dados do GET.
// (Ele será realocado enquanto chega resposta fração a fração.)
static char *get_response_buffer = NULL;
static int get_response_len = 0;

static void log_http_heap(const char *operacao) {
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    ESP_LOGI(TAGLogin, "%s heap livre=%u, minimo=%u, maior_bloco=%u", operacao, (unsigned)free_heap,
             (unsigned)min_free, (unsigned)largest);
}

static void clear_get_response_buffer(void) {
    if (get_response_buffer != NULL) {
        free(get_response_buffer);
        get_response_buffer = NULL;
    }
    get_response_len = 0;
}

bool verifica_conexao_internet() {
    struct addrinfo *res;
    int err = getaddrinfo("www.google.com", NULL, NULL, &res);
    if (err == 0 && res != NULL) {
        freeaddrinfo(res);
        return true;
    }
    return false;
}

static esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        char *tmp = realloc(response_buffer, response_len + evt->data_len + 1);
        if (tmp == NULL) {
            ESP_LOGE(TAGLogin, "Erro ao alocar memória para resposta.");
            return ESP_FAIL;
        }
        response_buffer = tmp;
        memcpy(response_buffer + response_len, evt->data, evt->data_len);
        response_len += evt->data_len;
        response_buffer[response_len] = '\0';
        break;
    default:
        break;
    }
    return ESP_OK;
}

bool fazer_login(const char *usuario, const char *senha) {
    char post_data[128];
    snprintf(post_data, sizeof(post_data), "{\"usuario\":\"%s\",\"password\":\"%s\"}", usuario, senha);

    free(response_buffer);
    response_buffer = NULL;
    response_len = 0;

    // printf("usuario %s senha %s\n", usuario, senha);

    esp_http_client_config_t config = {
        .url = MAIN_ROUTE "/users/login/",
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .cert_pem = rootCaCerticate,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
        .event_handler = _http_event_handler,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAGLogin, "Erro ao inicializar cliente HTTP.");
        return false;
    }

    esp_http_client_set_header(client, "X-Device-Name", "Connect");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Connection", "close");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    log_http_heap("Antes do login HTTPS");
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erro na requisição: %s", esp_err_to_name(err));
        log_http_heap("Falha no login HTTPS");
        esp_http_client_cleanup(client);
        free(response_buffer);
        response_buffer = NULL;
        response_len = 0;
        return false;
    }

    int status = esp_http_client_get_status_code(client);
    int content_length = esp_http_client_get_content_length(client);
    ESP_LOGI(TAGLogin, "Status: %d, Content-Length: %d", status, content_length);

    bool login_ok = (status == 200 && response_len > 0);

    if (response_len > 0) {
        // ESP_LOGI(TAG, "Resposta bruta do servidor (string): %.*s", response_len, response_buffer);

        cJSON *json = cJSON_Parse(response_buffer);
        if (json) {
            const cJSON *access_token = cJSON_GetObjectItem(json, "access");
            if (access_token && cJSON_IsString(access_token) && access_token->valuestring != NULL) {
                strncpy(token_global, access_token->valuestring, sizeof(token_global) - 1);
                token_global[sizeof(token_global) - 1] = '\0';
                ESP_LOGI(TAGLogin, "Token recebido e armazenado: %s", token_global);
            } else {
                ESP_LOGW(TAG, "Campo 'access' não encontrado ou inválido.");
                login_ok = false;
            }
            cJSON_Delete(json);
        } else {
            ESP_LOGW(TAG, "Resposta não é JSON válido.");
            login_ok = false;
        }
    } else {
        ESP_LOGW(TAG, "Nenhuma resposta recebida do servidor.");
    }

    if (response_buffer != NULL) {
        free(response_buffer);
        response_buffer = NULL;
        response_len = 0;
    }

    esp_http_client_cleanup(client);

    // ESP_LOGI(TAG, "Retornando login_ok: %d", login_ok);
    return login_ok;
}

int server_request(const char *url, const char *body, http_method_t method) {

    if (!url || strlen(token_global) == 0) {
        ESP_LOGE(TAGLogin, "server_request cancelado: url ou token ausente");
        return -1;
    }

    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s%s", MAIN_ROUTE, url);

    char *full_url = strdup(tmp);

    if (!full_url) {
        ESP_LOGE(TAGLogin, "Sem memória para full_url");
        return -1;
    }

    esp_http_client_config_t config = {
        .url = full_url,
        .method = (method == HTTP_POST) ? HTTP_METHOD_POST : HTTP_METHOD_PATCH,
        .timeout_ms = 15000, // Timeout de 5 segundos
        .buffer_size = 1024,
        // .buffer_size_tx = 2048,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
        .cert_pem = rootCaCerticate,
        .disable_auto_redirect = true,
        .event_handler = NULL,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAGLogin, "Erro ao inicializar cliente HTTP.");
        free(full_url); // ✅ evitar leak
        return -1;
    }

    // Configura cabeçalhos
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Connection", "close");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");

    // Cabeçalho de autenticação
    char auth_header[800];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", token_global);
    esp_http_client_set_header(client, "Authorization", auth_header);

    // Define o corpo da requisição
    // if (body != NULL)
    // {
    //     esp_http_client_set_post_field(client, body, strlen(body));
    // }

    if (body && strlen(body) > 0) {
        esp_err_t s = esp_http_client_set_post_field(client, body, strlen(body));
        if (s != ESP_OK) {
            ESP_LOGW(TAGLogin, "set_post_field falhou: %d", s);
            // continue mesmo assim
        }
    }

    // Executa a requisição
    log_http_heap((method == HTTP_POST) ? "Antes do POST HTTPS" : "Antes do PATCH HTTPS");
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAGLogin, "Erro na requisição: %s", esp_err_to_name(err));
        log_http_heap((method == HTTP_POST) ? "Falha no POST HTTPS" : "Falha no PATCH HTTPS");
        esp_http_client_cleanup(client);
        free(full_url);
        return -1;
    }

    // Obtém o código de status
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAGLogin, "Requisição %s para %s retornou status: %d", (method == HTTP_POST) ? "POST" : "PATCH", url,
             status);

    // Libera o cliente
    esp_http_client_cleanup(client);

    free(full_url); // ✅ libera na hora certa!

    if (status == 401 || status == 403) {
        token_global[0] = '\0';
        TokenOk = 0;
        if (Task_login_task)
            xTaskNotifyGive(Task_login_task);
    }

    return status;
}

static esp_err_t _http_event_handler_get(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        // Se for a primeira vez, malloc do tamanho inicial:
        if (get_response_buffer == NULL) {
            get_response_buffer = malloc(evt->data_len);
            if (get_response_buffer == NULL) {
                ESP_LOGE(TAG_GET, "Falha no malloc (tamanho pedido = %d)", evt->data_len);
                get_response_len = 0;
                return ESP_FAIL;
            }
            get_response_len = 0;
        } else {
            // Realoca para comportar len antiga + novo pedaço
            char *tmp = realloc(get_response_buffer, get_response_len + evt->data_len);
            if (tmp == NULL) {
                ESP_LOGE(TAG_GET, "Falha no realloc (tamanho pedido = %d)", get_response_len + evt->data_len);
                free(get_response_buffer);
                get_response_buffer = NULL;
                get_response_len = 0;
                return ESP_FAIL;
            }
            get_response_buffer = tmp;
        }
        // Copia os dados recebidos para o final do buffer
        memcpy(get_response_buffer + get_response_len, evt->data, evt->data_len);
        get_response_len += evt->data_len;
        break;

    default:
        break;
    }
    return ESP_OK;
}

/**
 * server_get_json:
 *   Faz um GET em `full_url` usando Bearer `token`.
 *   Retorna 1 em caso de sucesso (e preenche *out_buffer e *out_len), ou 0 em falha.
 *   O buffer retornado deve ser liberado pelo chamador (free()).
 *
 * @param full_url    : URL completa (e.g. "https://api.exemplo.com/estacoes/1")
 * @param out_buffer  : endereço de um char* que receberá ponteiro para JSON (terminado em '\0')
 * @param out_len     : endereço de um int para receber o tamanho em bytes (sem contar o '\0')
 *
 * @return 1 (sucesso) ou 0 (falha). Em falha, *out_buffer não é modificado.
 */

int server_get_json(const char *full_url, char **out_buffer, int *out_len) {
    // 1) Verifica parâmetros básicos
    if (full_url == NULL || strlen(full_url) == 0) {
        ESP_LOGE(TAGLogin, "URL inválida.");
        return 0;
    }

    if (strlen(token_global) == 0) {
        ESP_LOGE(TAGLogin, "Token de autenticação ausente. Requisição cancelada.");
        return -1;
    }

    // 2) Zera/limpa buffers globais usados pelo event_handler
    clear_get_response_buffer();

    // 3) Configura cliente HTTP
    esp_http_client_config_t config = {
        .url = full_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000, // Timeout de 2 segundos (ajuste se quiser mais)
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
        .disable_auto_redirect = true,
        .cert_pem = rootCaCerticate,
        .event_handler = _http_event_handler_get,
        .buffer_size = 1024, // internal buffer do TCP
        .buffer_size_tx = 512,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG_GET, "Falha ao inicializar esp_http_client.");
        return 0;
    }

    // Cabeçalho de autenticação
    char auth_header[800];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", token_global);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Connection", "close");

    // 5) Executa o GET (bloqueia até 2 s ou até responder)
    log_http_heap("Antes do GET HTTPS");
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_GET, "esp_http_client_perform() falhou: %s", esp_err_to_name(err));
        log_http_heap("Falha no GET HTTPS");
        esp_http_client_cleanup(client);
        clear_get_response_buffer();
        return 0;
    }

    // 6) Verifica status HTTP e se chegou algum dado
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG_GET, "GET retornou status %d", status);
        esp_http_client_cleanup(client);
        clear_get_response_buffer();
        return 0;
    }

    if (get_response_len == 0) {
        ESP_LOGW(TAG_GET, "GET devolveu corpo vazio.");
        esp_http_client_cleanup(client);
        clear_get_response_buffer();
        return 0;
    }

    // 7) Adiciona caractere nulo ao final, para ser string C
    //    Realoca mais 1 byte se necessário:
    char *tmp = realloc(get_response_buffer, get_response_len + 1);
    if (tmp == NULL) {
        ESP_LOGE(TAG_GET, "Falha no realloc final (+1 byte).");
        free(get_response_buffer);
        get_response_buffer = NULL;
        get_response_len = 0;
        esp_http_client_cleanup(client);
        return 0;
    }
    get_response_buffer = tmp;
    get_response_buffer[get_response_len] = '\0';

    // 8) Passa o ponteiro e o tamanho para o chamador
    *out_buffer = get_response_buffer;
    *out_len = get_response_len;
    get_response_buffer = NULL;
    get_response_len = 0;

    // 9) Limpa o handle do cliente (mas NÃO libera o get_response_buffer!)
    esp_http_client_cleanup(client);

    // 10) Retorna sucesso. O chamador DEVE chamar `free(*out_buffer)` quando não precisar mais.
    return 1;
}
