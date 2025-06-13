#ifndef NET_TASK_H
#define NET_TASK_H

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

// 任務與事件旗標
extern SemaphoreHandle_t xWifi;
extern TaskHandle_t xServerCheckHandle;
extern TaskHandle_t xServerLoginHandle;
extern QueueHandle_t net_queue;
extern EventGroupHandle_t net_event_group;

#define NET_WIFI_CONNECTED_BIT BIT0
#define NET_SERVER_CONNECTED_BIT BIT1
#define NET_TOKEN_AVAILABLE_BIT BIT2
#define NET_GOOGLE_TOKEN_AVAILABLE_BIT BIT3

// 網路事件結構
typedef struct net_event_t {
    const char *url;                 // 請求的完整 URL
    esp_http_client_method_t method; // HTTP 方法
    const char *post_data;           // POST 資料（可為 NULL）
    bool use_jwt;                    // 是否帶 JWT token
    bool save_to_buffer;             // 是否將回應寫入 buffer
    char *response_buffer;           // 回應 buffer（由呼叫者分配）
    size_t response_buffer_size;     // buffer 大小
    void (*on_finish)(struct net_event_t *event, esp_err_t result); // 完成 callback
    void *user_data;
    bool json_parse;  // 使用者自訂資料
    cJSON *json_root; // 若不為 NULL，則自動 parse JSON 並存於此
} net_event_t;

// 公用網路 worker task
void net_worker_task(void *pvParameters);

// 使用者設定下載
void userSettings(void);

// 狀態檢查
void server_check(void);

// 連線成功/失敗回呼
void cb_connection_ok(void *pvParameter);
void cb_button_wifi_settings(void *pvParameters);
void cb_button_continue_without_wifi(void *pvParameters);
void cb_button_setting_done(void *pvParameters);

// 伺服器連線測試
bool http_check_server_connectivity(esp_http_client_handle_t client);

// 網路啟動
void netStartup(void *pvParameters);

#endif // NET_TASK_H