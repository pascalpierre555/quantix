#include "font_task.h"
#include "esp_http_client.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char utf8_key[4];
    uint8_t data[FONT_SIZE];
} FontEntry;

FontEntry font_table[MAX_FONTS];
int font_table_count = 0;

// 將 UTF-8 hex 檔名還原成原始字串
bool hex_to_utf8(const char *hexname, char *utf8_out) {
    int len = strlen(hexname);
    if (len % 2 != 0 || len >= 8)
        return false;
    for (int i = 0; i < len / 2; ++i) {
        unsigned int byte;
        sscanf(hexname + 2 * i, "%2X", &byte);
        utf8_out[i] = byte;
    }
    utf8_out[len / 2] = '\0';
    return true;
}

void font_table_init(void) {
    DIR *dir = opendir(FONT_DIR);
    struct dirent *entry;
    font_table_count = 0;

    while ((entry = readdir(dir)) != NULL && font_table_count < MAX_FONTS) {
        if (entry->d_type != DT_REG)
            continue;

        char utf8[4];
        if (!hex_to_utf8(entry->d_name, utf8))
            continue;

        char path[272];
        snprintf(path, sizeof(path), "%s/%s", FONT_DIR, entry->d_name);
        FILE *fp = fopen(path, "rb");
        if (!fp)
            continue;

        FontEntry *e = &font_table[font_table_count++];
        strcpy(e->utf8_key, utf8);
        fread(e->data, 1, FONT_SIZE, fp);
        fclose(fp);
    }

    closedir(dir);
    ESP_LOGI("FONT", "Loaded %d fonts from LittleFS", font_table_count);
}

bool font_exists(const char *utf8_char) {
    for (int i = 0; i < font_table_count; ++i) {
        if (strcmp(font_table[i].utf8_key, utf8_char) == 0) {
            return true;
        }
    }
    return false;
}

// 取得 str 中所有未存在的字，存入 missing[]，每個字為 null-terminated UTF-8
int find_missing_characters(const char *str, char missing[][4], int max) {
    int count = 0;
    while (*str && count < max) {
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
            strcpy(missing[count++], utf8);
        }

        str += len;
    }

    return count;
}
