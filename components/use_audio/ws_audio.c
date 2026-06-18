#include "my_audio.h"
#include <cJSON.h>
#include "esp_websocket_client.h"
#include "esp_log.h"

#include "raw_stream.h"
#include "audio_element.h"
#include "esp_audio.h"

#include "app_events.h"
#include "esp_heap_caps.h"

/* ========== ✨新增：块池配置 ========== */
#define WS_CHUNK_SIZE    1024        // 每个块 1KB
#define WS_CHUNK_COUNT   128          // 共 16 块 = 128KB
#define WS_TASK_STACK    (3 * 1024)  // 喂流任务栈
#define WS_TASK_PRIO     5           // 喂流任务优先级

#define WEBSOCKET_URI "ws://8.162.21.140"
#define WEBSOCKET_PORT 8765

static const char *TAG = "web_socket";

esp_websocket_client_handle_t client;
audio_element_handle_t raw_read_el;



/* ========== ✨新增：块池相关全局变量 ========== */
typedef struct {
    uint8_t *data;      // 指向块池中的某一块
    int      len;       // 有效数据长度
} ws_chunk_t;

static uint8_t     *g_chunk_pool = NULL;    // PSRAM 中所有块的基址
static QueueHandle_t g_data_q    = NULL;    // 回调 → 喂流任务
static QueueHandle_t g_free_q    = NULL;    // 空闲块队列
static TaskHandle_t  g_feed_task = NULL;    // 喂流任务句柄


/* ========== ✨新增：块池初始化 ========== */
static esp_err_t chunk_pool_init(void)
{
    size_t total = WS_CHUNK_SIZE * WS_CHUNK_COUNT;

    g_chunk_pool = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (!g_chunk_pool) {
        ESP_LOGE(TAG, "chunk pool malloc(%d) failed", total);
        return ESP_ERR_NO_MEM;
    }

    g_data_q = xQueueCreate(WS_CHUNK_COUNT, sizeof(ws_chunk_t));
    g_free_q = xQueueCreate(WS_CHUNK_COUNT, sizeof(ws_chunk_t));
    if (!g_data_q || !g_free_q) {
        ESP_LOGE(TAG, "queue create failed");
        return ESP_ERR_NO_MEM;
    }

    // 所有块放入空闲队列
    for (int i = 0; i < WS_CHUNK_COUNT; i++) {
        ws_chunk_t c = { .data = g_chunk_pool + i * WS_CHUNK_SIZE, .len = 0 };
        xQueueSend(g_free_q, &c, 0);
    }

    ESP_LOGI(TAG, "chunk pool: %d x %d bytes = %d bytes",
             WS_CHUNK_COUNT, WS_CHUNK_SIZE, total);
    return ESP_OK;
}

/* ========== ✨新增：喂流任务 ========== */
static void feed_task(void *arg)
{
    ws_chunk_t c;

    while (1) {
        // 阻塞等数据
        if (xQueueReceive(g_data_q, &c, portMAX_DELAY) == pdPASS) {
            if (c.len > 0 && c.data != NULL) {
                // 写入音频管线（就算这里卡住，也不影响 WS 回调收 TCP）
                raw_stream_write(raw_read_el, (char *)c.data, c.len);
            }
            // 归还块
            xQueueSend(g_free_q, &c, 0);
        }
    }
}

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                     int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_BEGIN:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_BEGIN");
        break;
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        break;
    case WEBSOCKET_EVENT_DATA:
        /* ========== ✨改动：音频走块池，回调非阻塞 ========== */
        if (data->op_code == 0x2 || data->op_code == 0x0) {
            ws_chunk_t c;
            // 非阻塞取空闲块
            if (xQueueReceive(g_free_q, &c, 0) == pdPASS) {
                int copy = (data->data_len < WS_CHUNK_SIZE)
                         ? data->data_len : WS_CHUNK_SIZE;
                memcpy(c.data, data->data_ptr, copy);
                c.len = copy;
                // 非阻塞投递
                if (xQueueSend(g_data_q, &c, 100) != pdPASS) {
                    xQueueSend(g_free_q, &c, 100);  // 队列满，归还
                }
            }
            // 没空闲块 → 静默丢帧，回调不阻塞

        } else if (data->op_code == 0x08) {
            int code = 0;
            if (data->data_len >= 2) {
                code = 256 * data->data_ptr[0] + data->data_ptr[1];
            }
            ESP_LOGW(TAG, "Received close frame, code=%d", code);
        }
        /* ========== JSON 处理不变（只把 prebuf_reset 移到 if(root) 里）========== */
        if (data->op_code == 0x1) {
            cJSON *root = cJSON_ParseWithLength(data->data_ptr, data->data_len);
            if (root) {
                char *json_str = cJSON_PrintUnformatted(root);
                if (json_str) {
                    ESP_LOGI(TAG, "Parsed JSON: %s", json_str);
                    free(json_str);
                }
                if (cJSON_IsObject(root)) {
                    cJSON *temp = cJSON_GetObjectItem(root, "temp");
                    cJSON *time = cJSON_GetObjectItem(root, "time");
                    cJSON *funSpeed = cJSON_GetObjectItem(root, "funSpeed");
                    cJSON *food = cJSON_GetObjectItem(root, "food");
                    cJSON *action = cJSON_GetObjectItem(root, "action");
                    if (temp && cJSON_IsNumber(temp)) {
                        ESP_LOGI(TAG, "Temperature: %f", temp->valuedouble);
                    }
                    if (time && cJSON_IsNumber(time)) {
                        ESP_LOGI(TAG, "Time: %d", (int)time->valuedouble);
                    }
                    if (funSpeed && cJSON_IsNumber(funSpeed)) {
                        ESP_LOGI(TAG, "Fan Speed: %d", (int)funSpeed->valuedouble);
                    }
                    if (food && cJSON_IsString(food)) {
                        ESP_LOGI(TAG, "Food: %s", food->valuestring);
                    }
                    if (action && cJSON_IsString(action)) {
                        ESP_LOGI(TAG, "Action: %s", action->valuestring);
                    }
                    if (action && cJSON_IsString(action)
                        && strcmp(action->valuestring, "cook") == 0) {
                        cloud_data_t cook_json = {
                            .temperature = temp ? (float)temp->valuedouble : 0.0f,
                            .time_s = time ? (int)time->valuedouble : 0,
                            .fan_speed = funSpeed
                                ? (fan_speed_t)(int)funSpeed->valuedouble : fan_low,
                        };
                        if (food && cJSON_IsString(food)) {
                            snprintf(cook_json.food_name,
                                     sizeof(cook_json.food_name),
                                     "%s", food->valuestring);
                        }
                        esp_event_post_to(loop_handle, AIR_COOKER_EVENTS,
                                          EVENT_CLOUD_DATA, &cook_json,
                                          sizeof(cloud_data_t), 0);
                    }
                    if (action && cJSON_IsString(action)
                        && strcmp(action->valuestring, "start") == 0) {
                        cloud_cmd_t cmd = cloud_cmd_start;
                        esp_event_post_to(loop_handle, AIR_COOKER_EVENTS,
                                          EVENT_CLOUD_CMD, &cmd,
                                          sizeof(cloud_cmd_t), 0);
                    }
                    if (action && cJSON_IsString(action)
                        && strcmp(action->valuestring, "pause") == 0) {
                        cloud_cmd_t cmd = cloud_cmd_pause;
                        esp_event_post_to(loop_handle, AIR_COOKER_EVENTS,
                                          EVENT_CLOUD_CMD, &cmd,
                                          sizeof(cloud_cmd_t), 0);
                    }
                }
                cJSON_Delete(root);
            }
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_ERROR");
        log_error_if_nonzero("HTTP status code",
                             data->error_handle.esp_ws_handshake_status_code);
        if (data->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls",
                                 data->error_handle.esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack",
                                 data->error_handle.esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",
                                 data->error_handle.esp_transport_sock_errno);
        }
        break;
    case WEBSOCKET_EVENT_FINISH:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_FINISH");
        break;
    }
}

/* ========== ✨改动：init 中加块池和喂流任务 ========== */
void websocket_clint_init(void)
{
    // ── 块池 + 喂流任务（只初始化一次）──
    if (g_chunk_pool == NULL) {
        if (chunk_pool_init() != ESP_OK) return;
    }
    if (g_feed_task == NULL) {
        xTaskCreate(feed_task, "ws_feed",
                    WS_TASK_STACK, NULL,
                    WS_TASK_PRIO, &g_feed_task);
    }

    // ── WebSocket 连接 ──
    esp_websocket_client_config_t websocket_cfg = {
        .uri  = WEBSOCKET_URI,
        .port = WEBSOCKET_PORT,
    };

    client = esp_websocket_client_init(&websocket_cfg);

    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY,
                                   websocket_event_handler, (void *)client);

    esp_websocket_client_start(client);
}