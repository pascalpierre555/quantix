#ifndef FS_TASK_H
#define FS_TASK_H

#include "GUI_Paint.h"
#include "freertos/semphr.h" // For SemaphoreHandle_t
#include <esp_err.h>

#define FONT_DIR "/littlefs/fonts"
#define FONT_SIZE 32
#define MAX_FONTS 512
#define HEX_KEY_LEN 8

extern SemaphoreHandle_t xFontCacheMutex; // Mutex for font cache access
extern RTC_DATA_ATTR bool isr_woken;

void font_table_init(void);
int find_missing_characters(const char *str, char *missing, int max);
UWORD Paint_DrawString_Gen(UWORD x_start, UWORD y_start, UWORD area_width, UWORD area_height,
                           const char *text, sFONT *font, UWORD fg, UWORD bg);
esp_err_t download_missing_characters(const char *missing_chars);

#endif // FS_TASK_H