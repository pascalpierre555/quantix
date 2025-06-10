#ifndef FS_TASK_H
#define FS_TASK_H

#include <esp_err.h>

#define FONT_DIR "/littlefs/fonts"
#define FONT_SIZE 44
#define MAX_FONTS 512
#define HEX_KEY_LEN 8

void font_table_init(void);
int find_missing_characters(const char *str, char *missing, int max);
esp_err_t download_missing_characters(const char *missing_chars);

#endif // FS_TASK_H