#include "my_audio.h"
#include <cJSON.h>
#include "esp_websocket_client.h"
#include "esp_log.h"

#include "raw_stream.h"
#include "audio_element.h"
#include "esp_audio.h"

#include "app_events.h"
#include "esp_heap_caps.h"



#define WEBSOCKET_URI "ws://8.162.21.140"
#define WEBSOCKET_PORT 8765

static const char *TAG = "web_socket";

esp_websocket_client_handle_t client;
audio_element_handle_t raw_read_el;



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
        raw_stream_write(raw_read_el, data->data_ptr, data->data_len);
        } else if (data->op_code == 0x08) {
            int code = 0;
            if (data->data_len >= 2) {
                code = 256 * data->data_ptr[0] + data->data_ptr[1];
            }
            ESP_LOGW(TAG, "Received close frame, code=%d", code);
        }
        /* ========== JSON 命令解析：支持 type 字段（优先）和 action 字段（兼容） ========== */
        if (data->op_code == 0x1) {
            cJSON *root = cJSON_ParseWithLength(data->data_ptr, data->data_len);
            if (root) {
                char *json_str = cJSON_PrintUnformatted(root);
                if (json_str) {
                    ESP_LOGI(TAG, "Parsed JSON: %s", json_str);
                    free(json_str);
                }
                if (cJSON_IsObject(root)) {
                    // ── 提取通用字段 ──
                    cJSON *temp      = cJSON_GetObjectItem(root, "temp");
                    cJSON *time      = cJSON_GetObjectItem(root, "time");
                    cJSON *food      = cJSON_GetObjectItem(root, "food");
                    cJSON *fan_speed_json = cJSON_GetObjectItem(root, "fan_speed");
                    cJSON *funSpeed  = cJSON_GetObjectItem(root, "funSpeed");  // 兼容旧字段名

                    // ── 消息类型：优先读 type，兼容 action ──
                    cJSON *type_json  = cJSON_GetObjectItem(root, "type");
                    cJSON *action     = cJSON_GetObjectItem(root, "action");
                    const char *msg_type = NULL;
                    if (type_json && cJSON_IsString(type_json)) {
                        msg_type = type_json->valuestring;
                    } else if (action && cJSON_IsString(action)) {
                        msg_type = action->valuestring;  // 向后兼容
                    }

                    // ── 风扇转速：优先 fan_speed，兼容 funSpeed ──
                    int fan_val = -1;
                    if (fan_speed_json && cJSON_IsString(fan_speed_json)) {
                        if (strcmp(fan_speed_json->valuestring, "High") == 0)      fan_val = fan_high;
                        else if (strcmp(fan_speed_json->valuestring, "Medium") == 0) fan_val = fan_mid;
                        else if (strcmp(fan_speed_json->valuestring, "Low") == 0)   fan_val = fan_low;
                    } else if (funSpeed && cJSON_IsNumber(funSpeed)) {
                        fan_val = (int)funSpeed->valuedouble;  // 兼容旧数字格式
                    }

                    if (msg_type) {
                        ESP_LOGI(TAG, "Message type: %s", msg_type);

                        // ========== cook：开始烹饪 ==========
                        if (strcmp(msg_type, "cook") == 0) {
                            cloud_data_t cook_json = {
                                .temperature = temp ? (float)temp->valuedouble : 0.0f,
                                .time_s      = time ? (int)time->valuedouble : 0,
                                .fan_speed   = (fan_val >= 0) ? (fan_speed_t)fan_val : fan_low,
                                .food_name   = "",
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
                        // ========== start：直接开始烹饪（不带参数）==========
                        else if (strcmp(msg_type, "start") == 0) {
                            cloud_cmd_t cmd = cloud_cmd_start;
                            esp_event_post_to(loop_handle, AIR_COOKER_EVENTS,
                                              EVENT_CLOUD_CMD, &cmd,
                                              sizeof(cloud_cmd_t), 0);
                        }
                        // ========== stop：停止烹饪 ==========
                        else if (strcmp(msg_type, "stop") == 0) {
                            cloud_cmd_t cmd = cloud_cmd_stop;
                            esp_event_post_to(loop_handle, AIR_COOKER_EVENTS,
                                              EVENT_CLOUD_CMD, &cmd,
                                              sizeof(cloud_cmd_t), 0);
                        }
                        // ========== pause：暂停烹饪 ==========
                        else if (strcmp(msg_type, "pause") == 0) {
                            cloud_cmd_t cmd = cloud_cmd_pause;
                            esp_event_post_to(loop_handle, AIR_COOKER_EVENTS,
                                              EVENT_CLOUD_CMD, &cmd,
                                              sizeof(cloud_cmd_t), 0);
                        }
                        // ========== schedule：预约烹饪 ==========
                        else if (strcmp(msg_type, "schedule") == 0) {
                            schedule_data_t sched = {
                                .temperature = temp ? (float)temp->valuedouble : 0.0f,
                                .time_s      = time ? (int)time->valuedouble : 0,
                                .fan_speed   = (fan_val >= 0) ? (fan_speed_t)fan_val : fan_low,
                                .food_name   = "",
                                .scheduled_at = "",
                            };
                            if (food && cJSON_IsString(food)) {
                                snprintf(sched.food_name, sizeof(sched.food_name),
                                         "%s", food->valuestring);
                            }
                            cJSON *sched_at = cJSON_GetObjectItem(root, "scheduled_at");
                            if (sched_at && cJSON_IsString(sched_at)) {
                                snprintf(sched.scheduled_at, sizeof(sched.scheduled_at),
                                         "%s", sched_at->valuestring);
                            }
                            esp_event_post_to(loop_handle, AIR_COOKER_EVENTS,
                                              EVENT_CLOUD_SCHEDULE, &sched,
                                              sizeof(schedule_data_t), 0);
                        }
                        // ========== bound：设备已绑定 ==========
                        else if (strcmp(msg_type, "bound") == 0) {
                            cJSON *user_id = cJSON_GetObjectItem(root, "user_id");
                            if (user_id && cJSON_IsString(user_id)) {
                                ESP_LOGI(TAG, "Device bound to user: %s", user_id->valuestring);
                            }
                            esp_event_post_to(loop_handle, AIR_COOKER_EVENTS,
                                              EVENT_CLOUD_BOUND, NULL, 0, 0);
                        }
                        else {
                            ESP_LOGW(TAG, "Unknown message type: %s", msg_type);
                        }
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


void websocket_clint_init(void)
{
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
