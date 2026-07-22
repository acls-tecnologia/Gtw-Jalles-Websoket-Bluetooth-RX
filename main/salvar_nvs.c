#include <stdio.h>
#include "esp_system.h"
#include "nvs_flash.h"
#include "salvar_nvs.h"
#include "esp_log.h"

void nvs_salvar_float(char *float_key, float valor)
{
    nvs_handle_t nvs_handle;

    char Valor_str[25];

    snprintf(Valor_str, sizeof(Valor_str), "%f", valor); // Limitando a 6 casas decimais

    // Abre o namespace NVS
    ESP_ERROR_CHECK(nvs_open("armazenamento", NVS_READWRITE, &nvs_handle));

    // Salva o valor da variável na NVS
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, float_key, Valor_str));

    // Commit para gravar as mudanças
    ESP_ERROR_CHECK(nvs_commit(nvs_handle));

    // Fecha o handle da NVS
    nvs_close(nvs_handle);
}

float nvs_resgatar_float(char *key)
{
    nvs_handle_t nvs_handle;
    char Valor_str[25];
    size_t tamanho_valor_str = sizeof(Valor_str);
    float valor_f;

    ESP_ERROR_CHECK(nvs_open("armazenamento", NVS_READWRITE, &nvs_handle));

    esp_err_t ret = nvs_get_str(nvs_handle, key, Valor_str, &tamanho_valor_str);

    sscanf(Valor_str, "%f", &valor_f);

    if (ret == ESP_ERR_NVS_NOT_FOUND)
    {
        valor_f = -1; // Valor padrão
        ESP_LOGE("nvs_resgatar_float", "Valor não encontrado, retornando valor padrão = %f\n", valor_f);
    }
    else
    {
        ESP_ERROR_CHECK(ret);
    }

    nvs_close(nvs_handle);

    return valor_f;
}

/*#########################################  Funçoes de Configuraçao Bluetooth  ######################################### */

// Função para carregar o estado ID do GTW da NVS
int load_idGTW(void)
{
    nvs_handle_t my_handle;
    int32_t state = 0; // Valor padrão
    nvs_open("idGtw", NVS_READONLY, &my_handle);
    nvs_get_i32(my_handle, "idGtw", &state);
    nvs_close(my_handle);
    return state;
}

// Função para carregar o estado  ID da Unidade do GTW na NVS
int load_idUnidadeGTW(void)
{
    nvs_handle_t my_handle;
    int32_t state = 0; // Valor padrão
    nvs_open("idUnidadeGtw", NVS_READONLY, &my_handle);
    nvs_get_i32(my_handle, "idUnidadeGtw", &state);
    nvs_close(my_handle);
    return state;
}

// Função para carregar e salvar o nome do usuário do Gateway
void load_save_UsetGTW(char *device_name, size_t max_len)
{
    nvs_handle_t my_handle;
    esp_err_t err;

    // Abre o NVS
    err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE("NVS", "Erro ao abrir NVS!");
        return;
    }

    // Carrega o nome do usuário do Gateway
    size_t required_size;
    err = nvs_get_str(my_handle, "userGtw", NULL, &required_size);
    if (err == ESP_OK && required_size <= max_len)
    {
        nvs_get_str(my_handle, "userGtw", device_name, &required_size);
    }
    else
    {
        ESP_LOGE("NVS", "Erro ao carregar userGtw ou buffer insuficiente!");
    }

    // Fecha o NVS
    nvs_close(my_handle);
}

void load_PasswordGTW(char *password, size_t max_len)
{
    nvs_handle_t my_handle;
    esp_err_t err;

    // Abre o NVS
    err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE("NVS", "Erro ao abrir NVS para carregar a senha!");
        return;
    }

    // Carrega a senha associada à chave "passwordGtw"
    size_t required_size;
    err = nvs_get_str(my_handle, "passwordGtw", NULL, &required_size);
    if (err == ESP_OK && required_size <= max_len)
    {
        nvs_get_str(my_handle, "passwordGtw", password, &required_size);
    }
    else
    {
        ESP_LOGE("NVS", "Erro ao carregar passwordGtw ou buffer insuficiente!");
    }

    // Fecha o NVS
    nvs_close(my_handle);
}

// Função para carregar ea SIID do wifi na NVS
void load_save_SIIDWifi(char *device_name, size_t max_len)
{
    nvs_handle_t my_handle;
    esp_err_t err;

    // Abre o NVS
    err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE("NVS", "Erro ao abrir NVS!");
        return;
    }

    // Carrega o nome do usuário do Gateway
    size_t required_size;
    err = nvs_get_str(my_handle, "SSID", NULL, &required_size);
    if (err == ESP_OK && required_size <= max_len)
    {
        nvs_get_str(my_handle, "SSID", device_name, &required_size);
    }
    else
    {
        ESP_LOGE("NVS", "Erro ao carregar SSID ou buffer insuficiente!");
    }

    // Fecha o NVS
    nvs_close(my_handle);
}

void load_PasswordWifi(char *password, size_t max_len)
{
    nvs_handle_t my_handle;
    esp_err_t err;

    // Abre o NVS
    err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE("NVS", "Erro ao abrir NVS para carregar a senha Wifi!");
        return;
    }

    // Carrega a senha associada à chave "passwordGtw"
    size_t required_size;
    err = nvs_get_str(my_handle, "passwordWifi", NULL, &required_size);
    if (err == ESP_OK && required_size <= max_len)
    {
        nvs_get_str(my_handle, "passwordWifi", password, &required_size);
    }
    else
    {
        ESP_LOGE("NVS", "Erro ao carregar passwordWifi ou buffer insuficiente!");
    }

    // Fecha o NVS
    nvs_close(my_handle);
}
