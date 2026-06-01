#include "my_audio.h"
#include <cJSON.h>
#include "esp_websocket_client.h"
#include "esp_log.h"

#include "raw_stream.h"
#include "audio_element.h"
#include "esp_audio.h"

#include "app_events.h"

#define WEBSOCKET_URI "ws://192.168.57.157"
#define WEBSOCKET_PORT 8765



esp_websocket_client_handle_t client;
audio_element_handle_t raw_read_el;


static char *TAG = "web_socket";

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
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
        if (data->op_code == 0x2) { // Opcode 0x2 indicates binary data
            raw_stream_write(raw_read_el, (char *)data->data_ptr, data->data_len);
        } else if (data->op_code == 0x08 && data->data_len == 2) {
            ESP_LOGW(TAG, "Received closed message with code=%d", 256 * data->data_ptr[0] + data->data_ptr[1]);
        } 
        // If received data contains json structure it succeed to parse
        if (data->op_code == 0x1 ) { 
            cJSON *root = cJSON_Parse(data->data_ptr);
            if (root) {
                // 打印全部json数据以便调试
                char *json_str = cJSON_PrintUnformatted(root);
                if (json_str) {
                    ESP_LOGI(TAG, "Parsed JSON: %s", json_str);
                    free(json_str);
                }
                // 处理对象类型的响应（例如 {"status": "success", ...}）
                if (cJSON_IsObject(root)) {
                    // Handle object-type response (e.g. {"status": "success", ...})
                    cJSON *temp = cJSON_GetObjectItem(root, "temp");
                    cJSON *time = cJSON_GetObjectItem(root, "time");
                    cJSON *funSpeed = cJSON_GetObjectItem(root, "funSpeed");
                    if (temp && cJSON_IsNumber(temp)) {
                        ESP_LOGI(TAG, "Temperature: %f", temp->valuedouble);
                    }
                    if (time && cJSON_IsNumber(time)) {
                        ESP_LOGI(TAG, "Time: %d", (int)time->valuedouble);
                    }
                    if (funSpeed && cJSON_IsNumber(funSpeed)) {
                        ESP_LOGI(TAG, "Fan Speed: %d", (int)funSpeed->valuedouble);
                    }
                    cook_config_t cook_json = {
                        .temperature = temp ? (float)temp->valuedouble : 0.0f,
                        .time_s = time ? (int)time->valuedouble : 0,
                        .fan_speed = funSpeed ? (fan_speed_t)(int)funSpeed->valuedouble : fan_low
                    };
                    esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_CLOUD_CMD, &cook_json, sizeof(cook_config_t), portMAX_DELAY);
                }
                cJSON_Delete(root);
            }
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_ERROR");
        log_error_if_nonzero("HTTP status code",  data->error_handle.esp_ws_handshake_status_code);
        if (data->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", data->error_handle.esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", data->error_handle.esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  data->error_handle.esp_transport_sock_errno);
        }
        break;
    case WEBSOCKET_EVENT_FINISH:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_FINISH");
        break;
    }
}


void websocket_clint_init(void)
{
    esp_websocket_client_config_t websocket_cfg = {
        .uri = WEBSOCKET_URI,
        .port = WEBSOCKET_PORT,
    };

    client = esp_websocket_client_init(&websocket_cfg);

    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY,
                               websocket_event_handler, (void *)client);

    esp_websocket_client_start(client);

}
