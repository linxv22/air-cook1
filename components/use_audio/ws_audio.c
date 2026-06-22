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
        /* ========== JSON 命令解析：以 action 字段路由，funSpeed 为 int ========== */
        if (data->op_code == 0x1) {
            cJSON *root = cJSON_ParseWithLength(data->data_ptr, data->data_len);
            if (root) {
                char *json_str = cJSON_PrintUnformatted(root);
                if (json_str) {
                    ESP_LOGI(TAG, "Parsed JSON: %s", json_str);
                    free(json_str);
                }
                if (cJSON_IsObject(root)) {
                    // ── 消息路由：读 action 字段 ──
                    cJSON *action_json = cJSON_GetObjectItem(root, "action");
                    const char *action = NULL;
                    if (action_json && cJSON_IsString(action_json)) {
                        action = action_json->valuestring;
                    }

                    // ── 通用字段提取 ──
                    cJSON *temp_json   = cJSON_GetObjectItem(root, "temp");
                    cJSON *time_json   = cJSON_GetObjectItem(root, "time");
                    cJSON *food_json   = cJSON_GetObjectItem(root, "food");
                    cJSON *funSpeed    = cJSON_GetObjectItem(root, "funSpeed");
                    cJSON *reply_json  = cJSON_GetObjectItem(root, "reply");
                    const char *reply_str = (reply_json && cJSON_IsString(reply_json))
                                            ? reply_json->valuestring : NULL;

                    if (action == NULL) {
                        ESP_LOGW(TAG, "JSON missing 'action' field, ignored");
                        cJSON_Delete(root);
                        break;
                    }
                    ESP_LOGI(TAG, "Action: %s", action);

                    // ── 风扇转速：funSpeed 为 int（0=高, 1=中, 2=低）──
                    int fan_val = fan_low;
                    if (funSpeed && cJSON_IsNumber(funSpeed)) {
                        int fs = (int)funSpeed->valuedouble;
                        if (fs >= 0 && fs <= 2) fan_val = fs;
                    }

                    // ========== cook：开始烹饪 ==========
                    if (strcmp(action, "cook") == 0) {
                        cloud_data_t cook_json = {
                            .temperature = temp_json ? (float)temp_json->valuedouble : 0.0f,
                            .time_s      = time_json ? (int)time_json->valuedouble : 0,
                            .fan_speed   = (fan_speed_t)fan_val,
                            .food_name   = "",
                            .reply       = "",
                        };
                        if (food_json && cJSON_IsString(food_json)) {
                            snprintf(cook_json.food_name, sizeof(cook_json.food_name),
                                     "%s", food_json->valuestring);
                        }
                        if (reply_str) {
                            snprintf(cook_json.reply, sizeof(cook_json.reply),
                                     "%s", reply_str);
                        }
                        esp_event_post_to(loop_handle, AIR_COOKER_EVENTS,
                                          EVENT_CLOUD_DATA, &cook_json,
                                          sizeof(cloud_data_t), 0);
                    }
                    // ========== start：确认开始加热 ==========
                    else if (strcmp(action, "start") == 0) {
                        cloud_cmd_t cmd = cloud_cmd_start;
                        esp_event_post_to(loop_handle, AIR_COOKER_EVENTS,
                                          EVENT_CLOUD_CMD, &cmd,
                                          sizeof(cloud_cmd_t), 0);
                        if (reply_str) {
                            reply_data_t rd;
                            snprintf(rd.reply, sizeof(rd.reply), "%s", reply_str);
                            esp_event_post_to(loop_handle, AIR_COOKER_EVENTS,
                                              EVENT_CLOUD_REPLY, &rd,
                                              sizeof(reply_data_t), 0);
                        }
                    }
                    // ========== stop：停止烹饪 ==========
                    else if (strcmp(action, "stop") == 0) {
                        cloud_cmd_t cmd = cloud_cmd_stop;
                        esp_event_post_to(loop_handle, AIR_COOKER_EVENTS,
                                          EVENT_CLOUD_CMD, &cmd,
                                          sizeof(cloud_cmd_t), 0);
                        if (reply_str) {
                            reply_data_t rd;
                            snprintf(rd.reply, sizeof(rd.reply), "%s", reply_str);
                            esp_event_post_to(loop_handle, AIR_COOKER_EVENTS,
                                              EVENT_CLOUD_REPLY, &rd,
                                              sizeof(reply_data_t), 0);
                        }
                    }
                    // ========== pause：暂停烹饪 ==========
                    else if (strcmp(action, "pause") == 0) {
                        cloud_cmd_t cmd = cloud_cmd_pause;
                        esp_event_post_to(loop_handle, AIR_COOKER_EVENTS,
                                          EVENT_CLOUD_CMD, &cmd,
                                          sizeof(cloud_cmd_t), 0);
                        if (reply_str) {
                            reply_data_t rd;
                            snprintf(rd.reply, sizeof(rd.reply), "%s", reply_str);
                            esp_event_post_to(loop_handle, AIR_COOKER_EVENTS,
                                              EVENT_CLOUD_REPLY, &rd,
                                              sizeof(reply_data_t), 0);
                        }
                    }
                    // ========== schedule：预约烹饪 ==========
                    else if (strcmp(action, "schedule") == 0) {
                        schedule_data_t sched = {
                            .temperature = temp_json ? (float)temp_json->valuedouble : 0.0f,
                            .time_s      = time_json ? (int)time_json->valuedouble : 0,
                            .fan_speed   = (fan_speed_t)fan_val,
                            .food_name   = "",
                            .scheduled_at = "",
                        };
                        if (food_json && cJSON_IsString(food_json)) {
                            snprintf(sched.food_name, sizeof(sched.food_name),
                                     "%s", food_json->valuestring);
                        }
                        cJSON *sched_at = cJSON_GetObjectItem(root, "scheduled_at");
                        if (sched_at && cJSON_IsString(sched_at)) {
                            snprintf(sched.scheduled_at, sizeof(sched.scheduled_at),
                                     "%s", sched_at->valuestring);
                        }
                        esp_event_post_to(loop_handle, AIR_COOKER_EVENTS,
                                          EVENT_CLOUD_SCHEDULE, &sched,
                                          sizeof(schedule_data_t), 0);
                        if (reply_str) {
                            reply_data_t rd;
                            snprintf(rd.reply, sizeof(rd.reply), "%s", reply_str);
                            esp_event_post_to(loop_handle, AIR_COOKER_EVENTS,
                                              EVENT_CLOUD_REPLY, &rd,
                                              sizeof(reply_data_t), 0);
                        }
                    }
                    // ========== welcome：连接欢迎消息 ==========
                    else if (strcmp(action, "welcome") == 0) {
                        ESP_LOGI(TAG, "Welcome message received");
                        if (reply_str) {
                            reply_data_t rd;
                            snprintf(rd.reply, sizeof(rd.reply), "%s", reply_str);
                            esp_event_post_to(loop_handle, AIR_COOKER_EVENTS,
                                              EVENT_CLOUD_REPLY, &rd,
                                              sizeof(reply_data_t), 0);
                        }
                    }
                    // ========== chat：闲聊回复 ==========
                    else if (strcmp(action, "chat") == 0) {
                        ESP_LOGI(TAG, "Chat reply received");
                        if (reply_str) {
                            reply_data_t rd;
                            snprintf(rd.reply, sizeof(rd.reply), "%s", reply_str);
                            esp_event_post_to(loop_handle, AIR_COOKER_EVENTS,
                                              EVENT_CLOUD_REPLY, &rd,
                                              sizeof(reply_data_t), 0);
                        }
                    }
                    else {
                        ESP_LOGW(TAG, "Unknown action: %s", action);
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
