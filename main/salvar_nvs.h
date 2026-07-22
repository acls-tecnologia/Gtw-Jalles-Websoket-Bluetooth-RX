#if !defined __SALVAR_NVS_H__
#define __SALVAR_NVS_H__

typedef enum
{
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_CHAR
} DataType;

void nvs_salvar_float(char *float_key, float valor);

/**
 * @brief Salva na memória não volátil dados do tipo char (Função ainda não implementada).
 *
 * @param[out] char_key O nome de identificação desse dado na memória não volátil.
 * @param[in,out] valor o dado a ser armazenado na memória não volátil.
 * @return A função não retorna nenhum valor.
 */

float nvs_resgatar_float(char *key);

/**
 * @brief Resgata da memória não volátil o ultimo valor armazenado do tipo char através do identificador "key". (Função ainda não implementada).
 *
 * @param[out] key nome de identificação utilizado para armazenar o dado na memória.
 * @return O dado que foi armazenado na memória.
 * @retval O valor que foi armazenado, ou, em caso de falha, retorna um valor padrão = "".
 */

/*#########################################  Funçoes de Configuraçao Bluetooth  ######################################### */

int load_idGTW(void);

int load_idUnidadeGTW(void);

void load_save_UsetGTW(char *device_name, size_t max_len);

void load_PasswordGTW(char *password, size_t max_len);

void load_save_SIIDWifi(char *device_name, size_t max_len);

void load_PasswordWifi(char *password, size_t max_len);

#endif