#include "font_task.h"
#include "esp_http_client.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include <cJSON.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// 將單一 UTF-8 字（最多 3 bytes）轉為固定 6 字元 hex 字串
void utf8_to_hex(const char *utf8, char *hex_out, size_t hex_out_size) {
    // 取最多 3 bytes
    uint8_t bytes[3] = {0, 0, 0};
    int len = 0;
    for (; len < 3 && utf8[len]; ++len) {
        bytes[len] = (uint8_t)utf8[len];
    }
    // 固定輸出 6 字元
    snprintf(hex_out, hex_out_size, "%02x%02x%02x", bytes[0], bytes[1], bytes[2]);
}

typedef struct {
    char hex_key[HEX_KEY_LEN]; // hex 字串
    uint8_t data[FONT_SIZE];
} FontEntry;

#define HASH_TABLE_SIZE 4096

FontEntry font_table[MAX_FONTS];
int font_table_count = 0;

// hash table 結構
typedef struct {
    char hex_key[HEX_KEY_LEN];
    int table_index; // 指向 font_table 的 index
    bool used;
} FontHashEntry;

static FontHashEntry font_hash_table[HASH_TABLE_SIZE];

// hash function
static unsigned int hash_hex(const char *hex) {
    unsigned int h = 0;
    for (int i = 0; hex[i] && i < HEX_KEY_LEN - 1; ++i) {
        h = h * 31 + (unsigned char)hex[i];
    }
    return h % HASH_TABLE_SIZE;
}

// 插入 hash table
static void font_hash_insert(const char *hex, int table_index) {
    unsigned int idx = hash_hex(hex);
    for (int i = 0; i < HASH_TABLE_SIZE; ++i) {
        unsigned int try = (idx + i) % HASH_TABLE_SIZE;
        if (!font_hash_table[try].used) {
            strcpy(font_hash_table[try].hex_key, hex);
            font_hash_table[try].table_index = table_index;
            font_hash_table[try].used = true;
            break;
        }
    }
}

// 查找 hash table
static int font_hash_find(const char *hex) {
    unsigned int idx = hash_hex(hex);
    for (int i = 0; i < HASH_TABLE_SIZE; ++i) {
        unsigned int try = (idx + i) % HASH_TABLE_SIZE;
        if (!font_hash_table[try].used)
            return -1;
        if (strcmp(font_hash_table[try].hex_key, hex) == 0)
            return font_hash_table[try].table_index;
    }
    return -1;
}

// 將 hex 檔名還原成原始 UTF-8 字串
bool hex_to_utf8(const char *hexname, char *utf8_out) {
    int len = strlen(hexname);
    if (len % 2 != 0 || len >= HEX_KEY_LEN)
        return false;
    for (int i = 0; i < len / 2; ++i) {
        unsigned int byte;
        sscanf(hexname + 2 * i, "%2x", &byte);
        utf8_out[i] = byte;
    }
    utf8_out[len / 2] = '\0';
    return true;
}

void font_table_init(void) {
    DIR *dir = opendir(FONT_DIR);
    struct dirent *entry;
    font_table_count = 0;
    // 清空 hash table
    memset(font_hash_table, 0, sizeof(font_hash_table));

    if (!dir) {
        ESP_LOGW("FONT", "opendir failed for %s", FONT_DIR);
        return;
    }

    while ((entry = readdir(dir)) != NULL && font_table_count < MAX_FONTS) {
        if (entry->d_type != DT_REG)
            continue;

        char hex[HEX_KEY_LEN];
        strncpy(hex, entry->d_name, HEX_KEY_LEN - 1);
        hex[HEX_KEY_LEN - 1] = '\0';

        char path[272];
        snprintf(path, sizeof(path), "%s/%s", FONT_DIR, entry->d_name);
        FILE *fp = fopen(path, "rb");
        if (!fp)
            continue;

        FontEntry *e = &font_table[font_table_count];
        strcpy(e->hex_key, hex);
        fread(e->data, 1, FONT_SIZE, fp);
        fclose(fp);

        // 插入 hash table
        font_hash_insert(hex, font_table_count);

        font_table_count++;
    }

    closedir(dir);
    ESP_LOGI("FONT", "Loaded %d fonts from LittleFS", font_table_count);
}

bool font_exists(const char *utf8_char) {
    char hex[HEX_KEY_LEN];
    utf8_to_hex(utf8_char, hex, sizeof(hex));
    return font_hash_find(hex) >= 0;
}

// 取得 str 中所有未存在的字，所有缺字 hex 串接成一個字串
int find_missing_characters(const char *str, char *missing, int max_missing) {
    int count = 0;
    missing[0] = '\0'; // 初始化為空字串
    while (*str && count < max_missing) {
        int len = 1;
        if ((*str & 0xF0) == 0xF0)
            len = 4;
        else if ((*str & 0xE0) == 0xE0)
            len = 3;
        else if ((*str & 0xC0) == 0xC0)
            len = 2;

        char utf8[4] = {0};
        memcpy(utf8, str, len);

        if (!font_exists(utf8)) {
            char hex[HEX_KEY_LEN];
            utf8_to_hex(utf8, hex, sizeof(hex));
            // 串接到 missing 字串
            strncat(missing, hex, max_missing * HEX_KEY_LEN - strlen(missing) - 1);
            count++;
        }

        str += len;
    }
    return count;
}

esp_err_t download_missing_characters(const char *missing_chars) {
    char url[256];

    snprintf(url, sizeof(url), "https://peng-pc.tail941dce.ts.net/font?chars=%s", missing_chars);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err != ESP_OK) {
        ESP_LOGE("FONT", "HTTP GET failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_get_content_length(client);
    char *buffer = malloc(content_length + 1);
    esp_http_client_read(client, buffer, content_length);
    buffer[content_length] = '\0';
    esp_http_client_cleanup(client);

    // Parse JSON response
    cJSON *root = cJSON_Parse(buffer);
    if (!root) {
        ESP_LOGE("FONT", "JSON Parse error");
        free(buffer);
        return ESP_FAIL;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        const char *utf8_char = item->string;
        cJSON *bitmap_array = item;

        // 將 UTF-8 轉成 hex 檔名
        char filename[HEX_KEY_LEN] = {0};
        utf8_to_hex(utf8_char, filename, sizeof(filename));

        char path[64];
        snprintf(path, sizeof(path), FONT_DIR "/%s", filename);

        FILE *f = fopen(path, "wb");
        if (!f) {
            ESP_LOGE("FONT", "Failed to open file for writing: %s", path);
            continue;
        }

        int count = cJSON_GetArraySize(bitmap_array);
        uint8_t fontdata[FONT_SIZE] = {0};
        for (int i = 0; i < count && i < FONT_SIZE; i++) {
            fontdata[i] = (uint8_t)cJSON_GetArrayItem(bitmap_array, i)->valueint;
        }
        fwrite(fontdata, 1, FONT_SIZE, f);
        fclose(f);
        ESP_LOGI("FONT", "Saved: %s", path);
    }

    cJSON_Delete(root);
    free(buffer);
    return ESP_OK;
}
