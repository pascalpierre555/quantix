#include "font_task.h"
#include "esp_http_client.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h" // For portMAX_DELAY
#include "freertos/queue.h"    // For xQueueSend
#include "net_task.h"          // Required for net_event_t, net_queue
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

#define FONT_DIR_LEN (sizeof(FONT_DIR) - 1)
static const char *TAG_FONT = "FONT_TASK";

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
    ESP_LOGI(TAG_FONT, "Initializing font table from %s...", FONT_DIR);
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
    ESP_LOGI(TAG_FONT, "Loaded %d fonts from LittleFS into table.", font_table_count);
}

bool font_exists(const char *utf8_char) {
    char hex[HEX_KEY_LEN];
    utf8_to_hex(utf8_char, hex, sizeof(hex));
    return font_hash_find(hex) >= 0;
}

// 取得 str 中所有未存在的字，所有缺字 hex 串接成一個字串
int find_missing_characters(const char *str, char *missing, int missing_buffer_size) {
    int missing_chars_count = 0;
    missing[0] = '\0'; // 初始化為空字串
    size_t current_missing_hex_len = 0;
    const size_t single_hex_key_len = HEX_KEY_LEN - 1; // Length of "xxxxxx"

    while (*str) {
        int len = 1;
        // Determine UTF-8 character length
        if ((*str & 0xF0) == 0xF0) { // 4-byte UTF-8
            len = 4;
        } else if ((*str & 0xE0) == 0xE0) { // 3-byte UTF-8
            len = 3;
        } else if ((*str & 0xC0) == 0xC0) { // 2-byte UTF-8
            len = 2;
        }
        // else len = 1 for ASCII or invalid sequence start byte

        char utf8_char_bytes[5] = {0}; // Max 4 bytes for UTF-8 char + null terminator
        memcpy(utf8_char_bytes, str, len);

        // Skip English alphabet and digits
        if (len == 1) {
            char c = utf8_char_bytes[0];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                str += len;
                continue; // Skip this character
            }
        }

        if (!font_exists(utf8_char_bytes)) {
            // Check if there's enough space in the missing buffer for one more hex key + null
            // terminator
            if (current_missing_hex_len + single_hex_key_len + 1 <= missing_buffer_size) {
                char hex_key_output[HEX_KEY_LEN]; // Buffer for "xxxxxx\0"
                utf8_to_hex(utf8_char_bytes, hex_key_output, sizeof(hex_key_output));
                strcat(missing, hex_key_output); // Append the 6 hex characters
                current_missing_hex_len += single_hex_key_len;
                missing_chars_count++;
            } else {
                ESP_LOGW(TAG_FONT, "Missing characters hex string buffer full. Cannot add more.");
                break; // Stop if buffer is full
            }
        }

        str += len;
    }
    return missing_chars_count;
}

#define FONT_DOWNLOAD_BUFFER_SIZE 2048 // Adjust if necessary
static char font_download_response_buffer[FONT_DOWNLOAD_BUFFER_SIZE];

static void font_download_callback(net_event_t *event, esp_err_t result) {
    if (result == ESP_OK && event->json_root) {
        ESP_LOGI(TAG_FONT, "Font download successful, processing JSON response.");
        cJSON *font_item = NULL;
        int saved_count = 0;
        cJSON_ArrayForEach(font_item, event->json_root) {
            const char *hex_filename = font_item->string; // This is the hex key like "e4bda0"
            cJSON *bitmap_array = font_item;              // This is the cJSON array of bytes

            if (!cJSON_IsArray(bitmap_array)) {
                ESP_LOGW(TAG_FONT, "Bitmap data for %s is not an array.", hex_filename);
                continue;
            }

            char path[FONT_DIR_LEN + HEX_KEY_LEN + 2]; // FONT_DIR + "/" + hex_filename + "\0"
            snprintf(path, sizeof(path), "%s/%s", FONT_DIR, hex_filename);

            FILE *f = fopen(path, "wb");
            if (!f) {
                ESP_LOGE(TAG_FONT, "Failed to open file for writing: %s", path);
                continue;
            }

            uint8_t font_pixel_data[FONT_SIZE] = {
                0}; // Initialize to ensure padding if server sends less
            int bytes_in_array = cJSON_GetArraySize(bitmap_array);
            int bytes_to_process = (bytes_in_array < FONT_SIZE) ? bytes_in_array : FONT_SIZE;

            for (int i = 0; i < bytes_to_process; i++) {
                cJSON *byte_val_item = cJSON_GetArrayItem(bitmap_array, i);
                if (cJSON_IsNumber(byte_val_item)) {
                    font_pixel_data[i] = (uint8_t)byte_val_item->valueint;
                } else {
                    ESP_LOGW(TAG_FONT, "Invalid byte data in bitmap array for %s at index %d",
                             hex_filename, i);
                }
            }

            size_t written_count =
                fwrite(font_pixel_data, 1, FONT_SIZE, f); // Always write FONT_SIZE bytes
            fclose(f);

            if (written_count == FONT_SIZE) {
                ESP_LOGI(TAG_FONT, "Saved font: %s", path);
                saved_count++;
            } else {
                ESP_LOGE(TAG_FONT, "Failed to write complete font data for: %s (wrote %d/%d)", path,
                         written_count, FONT_SIZE);
            }
        }
        cJSON_Delete(event->json_root);
        event->json_root = NULL; // Mark as processed

        if (saved_count > 0) {
            ESP_LOGI(TAG_FONT, "Finished processing downloaded fonts. Re-initializing font table.");
            font_table_init(); // Reload fonts into memory
        }

    } else {
        ESP_LOGE(TAG_FONT, "Font download failed or JSON parse error. HTTP result: %s",
                 esp_err_to_name(result));
        if (event->response_buffer && strlen(event->response_buffer) > 0) {
            ESP_LOGE(TAG_FONT, "Response: %s", event->response_buffer);
        }
        if (event->json_root) { // Should be null if result != ESP_OK from net_worker_task
                                // perspective
            cJSON_Delete(event->json_root);
            event->json_root = NULL;
        }
    }
}

// Queues a request to download missing font characters.
esp_err_t download_missing_characters(const char *missing_chars) {
    if (missing_chars == NULL || strlen(missing_chars) == 0) {
        ESP_LOGI(TAG_FONT, "No missing characters to download.");
        return ESP_OK;
    }

    static char url[256]; // Static to be safe if net_event_t is copied shallowly by queue
    snprintf(url, sizeof(url), "https://peng-pc.tail941dce.ts.net/font?chars=%s", missing_chars);
    ESP_LOGI(TAG_FONT, "Requesting missing fonts from: %s", url);

    net_event_t font_event = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .post_data = NULL,
        .use_jwt = false,
        .save_to_buffer = true,
        .response_buffer = font_download_response_buffer,
        .response_buffer_size = sizeof(font_download_response_buffer),
        .on_finish = font_download_callback,
        .user_data = NULL,
        .json_root = (void *)1, // Request net_worker_task to parse JSON
    };

    // Ensure font_download_response_buffer is clean before use
    font_download_response_buffer[0] = '\0';

    if (xQueueSend(net_queue, &font_event, pdMS_TO_TICKS(1000)) != pdPASS) {
        ESP_LOGE(TAG_FONT, "Failed to send font download request to net_queue.");
        return ESP_FAIL;
    }
    return ESP_OK;
}
