#include "font_task.h"
#include "GUI_Paint.h"
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

// Log tag
static const char *TAG_FONT = "FONT_TASK";

// 字型目錄長度 (不含結尾空字符)
#define FONT_DIR_LEN (sizeof(FONT_DIR) - 1)
// 字型hash table大小
#define HASH_TABLE_SIZE 512
// 字型下載緩衝區大小 (如果需要可以調整)
#define FONT_DOWNLOAD_BUFFER_SIZE 4096

// 字型條目結構，存儲字型的十六進位key和像素數據
typedef struct {
    char hex_key[HEX_KEY_LEN]; // Hex string (e.g., "e4b8ad")
    uint8_t data[FONT_SIZE];   // 字型像素數據 (點陣)
} FontEntry;

// 字型hash table條目結構
typedef struct {
    char hex_key[HEX_KEY_LEN]; // Hex key
    int table_index;           // 指向 font_table 的索引
    bool used;                 // Flag indicating if this slot is used
} FontHashEntry;

int font_table_count = 0; // Current number of fonts loaded into font_table

static char
    font_download_response_buffer[FONT_DOWNLOAD_BUFFER_SIZE]; // Font download response buffer
FontEntry font_table[MAX_FONTS];                              // 字型數據表 (RAM 緩存)
static FontHashEntry font_hash_table[HASH_TABLE_SIZE]; // 字型hash table，用於快速查找

// 將單個 UTF-8 字元 (最多3字節) 轉換為固定的6字元十六進位string
void utf8_to_hex(const char *utf8, char *hex_out, size_t hex_out_size) {
    // 最多取3個字節
    uint8_t bytes[3] = {0, 0, 0};
    int len = 0;
    for (; len < 3 && utf8[len]; ++len) {
        bytes[len] = (uint8_t)utf8[len];
    }
    // 固定輸出6個十六進位字符
    snprintf(hex_out, hex_out_size, "%02x%02x%02x", bytes[0], bytes[1], bytes[2]);
}

// hash function，用於將十六進位key映射到hash table索引
static unsigned int hash_hex(const char *hex) {
    unsigned int h = 0;
    for (int i = 0; hex[i] && i < HEX_KEY_LEN - 1; ++i) {
        h = h * 31 + (unsigned char)hex[i];
    }
    return h % HASH_TABLE_SIZE;
}

// 向hash table中插入一個條目 (線性探測法處理衝突)
static void font_hash_insert(const char *hex, int table_index) {
    unsigned int idx = hash_hex(hex);
    for (int i = 0; i < HASH_TABLE_SIZE; ++i) {
        // 線性探測
        unsigned int try = (idx + i) % HASH_TABLE_SIZE;
        if (!font_hash_table[try].used) {
            strcpy(font_hash_table[try].hex_key, hex);
            font_hash_table[try].table_index = table_index;
            font_hash_table[try].used = true;
            break;
        }
    }
}

// 在hash table中查找十六進位key對應的 font_table 索引
static int font_hash_find(const char *hex) {
    unsigned int idx = hash_hex(hex);
    for (int i = 0; i < HASH_TABLE_SIZE; ++i) {
        unsigned int try = (idx + i) % HASH_TABLE_SIZE;
        // 如果位置未使用，則表示key不存在
        if (!font_hash_table[try].used)
            return -1;
        if (strcmp(font_hash_table[try].hex_key, hex) == 0)
            return font_hash_table[try].table_index;
    }
    return -1;
}

// 將十六進位文件名 (例如 "e4b8ad") 還原為原始 UTF-8 string
bool hex_to_utf8(const char *hexname, char *utf8_out) {
    int len = strlen(hexname);
    if (len % 2 != 0 || len >= HEX_KEY_LEN)
        // 十六進位string長度必須是偶數且小於 HEX_KEY_LEN
        return false;
    for (int i = 0; i < len / 2; ++i) {
        unsigned int byte;
        sscanf(hexname + 2 * i, "%2x", &byte);
        utf8_out[i] = byte;
    }
    utf8_out[len / 2] = '\0';
    return true;
}

// 初始化字型表，從 LittleFS 加載字型到 RAM
void font_table_init(void) {
    DIR *dir = opendir(FONT_DIR);
    struct dirent *entry;
    font_table_count = 0;
    // 清空hash table
    memset(font_hash_table, 0, sizeof(font_hash_table));
    ESP_LOGI(TAG_FONT, "Initializing font table from %s...", FONT_DIR);
    if (!dir) {
        ESP_LOGW(TAG_FONT, "Failed to open directory %s", FONT_DIR);
        // Should continue even if directory opening fails, as fonts might be downloaded later
        // return; // 不應在此處返回，允許程序繼續嘗試下載字型
    }

    // while ((entry = readdir(dir)) != NULL && font_table_count < MAX_FONTS) {
    //     if (entry->d_type != DT_REG)
    //         continue;

    //     char hex[HEX_KEY_LEN];
    //     strncpy(hex, entry->d_name, HEX_KEY_LEN - 1);
    //     hex[HEX_KEY_LEN - 1] = '\0';

    //     char path[272];
    //     snprintf(path, sizeof(path), "%s/%s", FONT_DIR, entry->d_name);
    //     FILE *fp = fopen(path, "rb");
    //     if (!fp)
    //         continue;

    //     FontEntry *e = &font_table[font_table_count];
    //     strcpy(e->hex_key, hex);
    //     fread(e->data, 1, FONT_SIZE, fp);
    //     fclose(fp);

    //     // 插入 hash table
    //     font_hash_insert(hex, font_table_count);

    //     font_table_count++;
    // }

    if (dir) { // 僅當目錄成功打開時才關閉
        closedir(dir);
    }
    // 初始加載時，font_table_count 應為 0，因為我們清除了hash table並且沒有實際從文件系統加載。
    // 實際的加載發生在 find_missing_characters 中。
    ESP_LOGI(TAG_FONT, "Font table initialization complete. Current RAM cached fonts: %d",
             font_table_count);
}

// 查找輸入string str 中所有本地不存在 (RAM 和 LittleFS 均沒有) 的字元。
// 將所有缺失字元的十六進位表示串聯成一個string，存儲在 missing 中。
// 返回缺失字元的數量。
int find_missing_characters(const char *str, char *missing, int missing_buffer_size) {
    if (strlen(str) > HASH_TABLE_SIZE - font_table_count) {
        // 如果預期處理的字元數可能導致hash table飽和，則清空hash table
        // 這是一個簡化的策略，可能導致不必要的重新加載
        memset(font_hash_table, 0, sizeof(font_hash_table)); // Clear hash table
        ESP_LOGI(TAG_FONT, "Font hash table cleared due to potential overflow.");
    }
    int missing_chars_count = 0;
    missing[0] = '\0'; // 初始化為空string
    size_t current_missing_hex_len = 0;
    const size_t single_hex_key_len = HEX_KEY_LEN - 1; // "xxxxxx" 的長度

    char utf8_char_bytes[5];          // UTF-8 字元最多4字節 + 空終止符
    char hex_key_output[HEX_KEY_LEN]; // "xxxxxx\0" 的緩衝區

    while (*str) {
        memset(utf8_char_bytes, 0, sizeof(utf8_char_bytes));
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

        memcpy(utf8_char_bytes, str, len);

        // 跳過英文字母和數字 (ASCII 可顯示字符)
        if (len == 1) {
            char c = utf8_char_bytes[0];
            if (c >= ' ' && c <= '~') {
                str += len;
                continue; // Skip this character
            }
        }

        // 1. 將當前 UTF-8 字元轉換為十六進位
        utf8_to_hex(utf8_char_bytes, hex_key_output, sizeof(hex_key_output));

        // 2. 檢查字型是否已在 RAM 緩存中
        if (font_hash_find(hex_key_output) >= 0) {
            str += len;
            continue; // 已在 RAM 中
        }

        // 3. 字型不在 RAM 中，檢查 LittleFS
        char lfs_font_path[FONT_DIR_LEN + 1 /* / */ + (HEX_KEY_LEN - 1) /* xxxxxx */ + 1 /* \0 */];
        snprintf(lfs_font_path, sizeof(lfs_font_path), "%s/%s", FONT_DIR, hex_key_output);

        FILE *fp = fopen(lfs_font_path, "rb");
        if (fp) { // 字型存在於 LittleFS
            if (font_table_count < MAX_FONTS) {
                // 從 LittleFS 加載到 RAM
                FontEntry *e = &font_table[font_table_count];
                strcpy(e->hex_key, hex_key_output); // hex_key_output 已是空終止的
                size_t bytes_read = fread(e->data, 1, FONT_SIZE, fp);
                fclose(fp);

                if (bytes_read == FONT_SIZE) {
                    font_hash_insert(hex_key_output, font_table_count);
                    font_table_count++;
                    ESP_LOGI(TAG_FONT,
                             "Loaded font %s (%s) from LittleFS to RAM. Cache size: %d/%d",
                             utf8_char_bytes, hex_key_output, font_table_count, MAX_FONTS);
                } else {
                    ESP_LOGE(TAG_FONT,
                             "Failed to read full font data for %s from %s. Read %zu bytes. Adding "
                             "to missing list.",
                             hex_key_output, lfs_font_path, bytes_read);
                    // 字型文件可能已損壞或不完整。視為缺失以便下載。
                    if (current_missing_hex_len + single_hex_key_len + 1 <=
                        (size_t)missing_buffer_size) {
                        strcat(missing, hex_key_output);
                        current_missing_hex_len += single_hex_key_len;
                        missing_chars_count++;
                    } else {
                        ESP_LOGW(TAG_FONT, "Missing characters hex string buffer full when adding "
                                           "corrupted LFS font.");
                    }
                }
            } else { // RAM 緩存已滿
                fclose(fp);
                ESP_LOGI(TAG_FONT,
                         "Font %s (%s) on LittleFS, but RAM cache full (MAX_FONTS=%d). Not loading "
                         "to RAM.",
                         utf8_char_bytes, hex_key_output, MAX_FONTS);
                // Font exists on disk, so it's not "missing" for download purposes.
                // 字型存在於磁盤上，因此對於下載目的而言並非“缺失”。
            }
            str += len;
            continue; // 已處理此字元 (已加載到 RAM 或在 LFS 上找到但 RAM 已滿)
        }

        // 4. 字型不在 RAM 中也不在 LittleFS 上 - 真正缺失
        ESP_LOGI(TAG_FONT, "Font %s (%s) not in RAM or LittleFS. Adding to download list.",
                 utf8_char_bytes, hex_key_output);
        if (current_missing_hex_len + single_hex_key_len + 1 <= (size_t)missing_buffer_size) {
            // 檢查 missing 緩衝區中是否有足夠的空間容納一個十六進位key + 空終止符
            strcat(missing, hex_key_output); // 附加6個十六進位字符
            current_missing_hex_len += single_hex_key_len;
            missing_chars_count++;
        } else {
            ESP_LOGW(TAG_FONT, "Missing characters hex string buffer full. Cannot add more.");
            // 不中斷；繼續處理 str 的其餘部分，以允許為其他字元加載 LFS
        }

        str += len;
    }
    return missing_chars_count;
}

// 字型下載完成後的回調函數
static void font_download_callback(net_event_t *event, esp_err_t result) {
    if (result == ESP_OK && event->json_root) {
        ESP_LOGI(TAG_FONT, "Response: %s", event->response_buffer);
        ESP_LOGI(TAG_FONT, "Font download successful, processing JSON response.");
        cJSON *font_item = NULL;
        int saved_count = 0;
        cJSON_ArrayForEach(font_item, event->json_root) {
            const char *hex_filename = font_item->string; // 這是十六進位key，例如 "e4bda0"
            cJSON *bitmap_array = font_item;              // 這是字節的 cJSON 陣列

            if (!cJSON_IsArray(bitmap_array)) {
                ESP_LOGW(TAG_FONT, "Bitmap data for %s is not an array.", hex_filename);
                continue;
            }

            // 構造字型檔案路徑: FONT_DIR/hex_filename
            char path[FONT_DIR_LEN + HEX_KEY_LEN + 2];
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

            // 始終寫入 FONT_SIZE 字節
            size_t written_count = fwrite(font_pixel_data, 1, FONT_SIZE, f);
            fclose(f);

            if (written_count == FONT_SIZE) {
                ESP_LOGI(TAG_FONT, "Saved font %s to LittleFS.", path);
                saved_count++;

                // Attempt to load into RAM cache if space is available
                if (font_table_count < MAX_FONTS) {
                    FontEntry *ram_entry = &font_table[font_table_count];
                    strcpy(ram_entry->hex_key, hex_filename);
                    memcpy(ram_entry->data, font_pixel_data, FONT_SIZE); // Use the downloaded data
                    font_hash_insert(hex_filename, font_table_count);    // Add to hash table
                    font_table_count++; // Increment RAM cache count
                    ESP_LOGI(TAG_FONT, "Loaded font %s into RAM. Cache size: %d/%d", hex_filename,
                             font_table_count, MAX_FONTS);
                } else {
                    ESP_LOGW(TAG_FONT,
                             "RAM cache full. Font %s saved to LittleFS but not loaded to RAM.",
                             hex_filename);
                }
            } else {
                ESP_LOGE(TAG_FONT,
                         "Failed to write complete font data for %s to LittleFS (wrote %zu/%d)",
                         path, written_count, FONT_SIZE);
            }
        }
        cJSON_Delete(event->json_root);
        event->json_root = NULL; // 標記為已處理

        if (saved_count > 0) {
            ESP_LOGI(TAG_FONT, "Finished processing and caching %d downloaded fonts.", saved_count);
        }

    } else {
        ESP_LOGE(TAG_FONT, "Font download failed or JSON parse error. HTTP result: %s",
                 esp_err_to_name(result));
        // 如果 result != ESP_OK，從 net_worker_task 的角度來看，json_root 應該為 null
        if (event->json_root) {
            cJSON_Delete(event->json_root);
            event->json_root = NULL;
        }
    }
}

// 通用繪製string函數，支持中英文混合，自動換行
UWORD Paint_DrawString_Gen(UWORD x_start, UWORD y_start, UWORD area_width, UWORD area_height,
                           const char *text, sFONT *font, UWORD fg, UWORD bg) {
    UWORD current_x = x_start;
    UWORD current_y = y_start;
    const char *p_text = text;

    if (!text || !font) {
        ESP_LOGE(TAG_FONT, "Paint_DrawString_Gen: Invalid arguments.");
        return 0;
    }

    UWORD eng_char_width = font->Width;
    UWORD char_height = font->Height;
    UWORD cn_char_layout_width = font->Width * 2; // Layout width for Chinese char

    // 根據中文字型的尺寸預先計算預期的 FONT_SIZE
    // 這對於正確解釋 FONT_SIZE 數據至關重要
    UWORD cn_char_pixel_width_from_font = font->Width * 2;
    UWORD cn_bytes_per_row_expected = (cn_char_pixel_width_from_font + 7) / 8;
    UWORD expected_font_size_for_cn = font->Height * cn_bytes_per_row_expected;

    if (FONT_SIZE != expected_font_size_for_cn) {
        ESP_LOGW(
            TAG_FONT,
            "FONT_SIZE (%d) mismatch with expected for Chinese char (%dx%d based on sFONT: %d). ",
            FONT_SIZE, char_height, cn_char_pixel_width_from_font, expected_font_size_for_cn);
    }

    while (*p_text != '\0') {
        if (current_y + char_height > y_start + area_height) {
            break; // No more vertical space
        }

        // 檢查是否為 ASCII 字元 (單字節)
        if ((*p_text & 0x80) == 0 && *p_text >= ' ' && *p_text <= '~') {
            if (*p_text == ' ') {
                // 處理空格：如果空格放不下，則換行。如果是新行的開頭空格，則跳過繪製。
                if (current_x + eng_char_width >
                    x_start + area_width) {   // 當前行剩餘空間不足以容納空格
                    current_x = x_start;      // X座標回到行首
                    current_y += char_height; // Y座標換到下一行
                    if (current_y + char_height > y_start + area_height)
                        break; // 超出繪圖區域，停止
                    // 此時 current_x == x_start，是新行的開頭，所以不繪製這個空格
                } else {
                    // 只有當空格不是新行的第一個字元時才繪製 (除非它是整個文本的第一個字元)
                    if (current_x != x_start || (p_text == text && current_y == y_start)) {
                        Paint_DrawChar(current_x, current_y, *p_text, font, fg, bg);
                        current_x += eng_char_width;
                    }
                }
                p_text++; // 指向下一個字元
            } else {
                // 非空格的 ASCII 字元 (單字的一部分)
                const char *word_scan_ptr = p_text;
                int word_len = 0;
                UWORD current_word_total_pixel_width = 0;

                // 掃描當前單字以獲取其長度和像素寬度
                while (*word_scan_ptr != '\0' && (*word_scan_ptr & 0x80) == 0 &&
                       *word_scan_ptr > ' ' && *word_scan_ptr <= '~') {
                    current_word_total_pixel_width += eng_char_width;
                    word_len++;
                    word_scan_ptr++;
                }

                // 1. 檢查整個單字是否能放在當前行。
                //    如果放不下，並且當前不是行的開頭，則先換行。
                if (current_x != x_start &&
                    (current_x + current_word_total_pixel_width > x_start + area_width)) {
                    current_x = x_start;
                    current_y += char_height;
                    if (current_y + char_height > y_start + area_height)
                        break;
                }

                // 2. 逐字元繪製單字。
                //    如果單字本身比一行還長 (area_width)，則在需要換行的地方加上連字號。
                for (int i = 0; i < word_len; ++i) {
                    // 檢查是否需要在繪製當前字元 *之前* 放置連字號並換行
                    // 條件：
                    //   a) 原始單字寬度大於區域寬度 (current_word_total_pixel_width > area_width)
                    //   b) 當前不是單字的第一個字元 (i > 0) 或 當前X座標不是行首 (current_x !=
                    //   x_start) c) 繪製當前字元會超出邊界 (current_x + eng_char_width > x_start +
                    //   area_width) d) 還有空間放連字號 (current_x + eng_char_width (for '-') <=
                    //   x_start + area_width)
                    //      (實際上，如果 (c) 成立，我們應該在 *前一個*
                    //      字元後加連字號，然後換行，再畫當前字元)
                    //      簡化：如果當前字元放不下，且是長單字的一部分，則在前一個位置考慮加連字號。
                    //      這裡的邏輯是：如果下一個字元會導致溢出，並且這是長單字的一部分，則在此處加連字號。

                    if (current_word_total_pixel_width > area_width && // 原始單字比一行長
                        (i < word_len) && // 確保 p_text[0] (即 *p_text) 是有效的
                        (current_x + eng_char_width > x_start + area_width) && // 當前字元會導致溢出
                        current_x != x_start) { // 並且不是在行首（行首溢出表示area_width太小）

                        Paint_DrawChar(current_x, current_y, '-', font, fg, bg); // 畫連字號
                        // current_x += eng_char_width; // 連字號佔用寬度 (如果需要獨立計算)

                        current_x = x_start;      // X座標回到行首
                        current_y += char_height; // Y座標換到下一行
                        if (current_y + char_height > y_start + area_height) {
                            p_text += (word_len - i);
                            goto end_of_string_loop_label;
                        } // 跳出外層循環
                    } else if (current_x + eng_char_width >
                               x_start + area_width) { // 普通換行（如果單字不長，或在行首）
                        current_x = x_start;
                        current_y += char_height;
                        if (current_y + char_height > y_start + area_height) {
                            p_text += (word_len - i);
                            goto end_of_string_loop_label;
                        }
                    }

                    Paint_DrawChar(current_x, current_y, *p_text, font, fg, bg);
                    current_x += eng_char_width;
                    p_text++; // 指向下一個字元
                }
            }
        } else { // UTF-8 多字節字元 (假定為中文)
            if (current_x + cn_char_layout_width > x_start + area_width) {
                current_x = x_start;
                current_y += char_height;
                // 換行
                if (current_y + char_height > y_start + area_height)
                    break;
            }

            char utf8_char_bytes[5] = {0};
            char hex_key_output[HEX_KEY_LEN];
            int utf8_len = 1;
            // 判斷 UTF-8 字元長度
            if ((*p_text & 0xF0) == 0xF0)
                utf8_len = 4;
            else if ((*p_text & 0xE0) == 0xE0)
                utf8_len = 3;
            else if ((*p_text & 0xC0) == 0xC0)
                utf8_len = 2;

            memcpy(utf8_char_bytes, p_text, utf8_len);
            utf8_to_hex(utf8_char_bytes, hex_key_output, sizeof(hex_key_output));

            int table_idx = font_hash_find(hex_key_output);
            if (table_idx >= 0 && table_idx < font_table_count) {
                // 使用 font_table 中的特定點陣圖繪製中文字元
                // 用於繪製的實際像素寬度來自 cn_char_pixel_width_from_font
                Paint_DrawBitMap_Paste(font_table[table_idx].data, current_x, current_y,
                                       cn_char_layout_width, char_height, 1);
            } else {
                // 未找到字型，繪製空心矩形
                Paint_DrawRectangle(
                    current_x + 1, current_y + 1, current_x + cn_char_layout_width - 2,
                    current_y + char_height - 2, fg, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
                ESP_LOGW(TAG_FONT, "Chinese font %s (hex: %s) not found in RAM cache.",
                         utf8_char_bytes, hex_key_output);
            }
            current_x += cn_char_layout_width;
            p_text += utf8_len;
        }
    }
end_of_string_loop_label:; // 標籤用於 goto 跳轉
    return ((current_y - y_start) / font->Height) + 1;
}

// 將下載缺失字型字元的請求加入queue。
esp_err_t download_missing_characters(const char *missing_chars) {
    if (missing_chars == NULL || strlen(missing_chars) == 0) {
        ESP_LOGI(TAG_FONT, "No missing characters to download.");
        return ESP_OK;
    }

    // 使用靜態 URL 緩衝區
    // URL 最大長度需要考慮基礎 URL 和 missing_chars 的長度
    static char url[256];
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
        .json_parse = 1, // 請求 net_worker_task 解析 JSON
    };

    // 使用前確保 font_download_response_buffer 是乾淨的
    font_download_response_buffer[0] = '\0';

    if (xQueueSend(net_queue, &font_event, pdMS_TO_TICKS(1000)) != pdPASS) {
        ESP_LOGE(TAG_FONT, "Failed to send font download request to net_queue.");
        return ESP_FAIL;
    }
    return ESP_OK;
}
