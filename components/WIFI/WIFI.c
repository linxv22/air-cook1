#include "WIFI.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_dpp.h"
#include "qrcode.h"
#include "esp_sntp.h"
#include "esp_netif_sntp.h"

#include "app_events.h"

#include "lwip/err.h"
#include "lwip/sys.h"

/* ========== 新增：本地 URI 状态与缓存 ========== */
static char s_dpp_uri_buffer[256] = {0};

#define WIFI_MAX_RETRY_NUM          5

#define DPP_DEVICE_INFO ""
#define DPP_BOOTSTRAPPING_KEY ""
#define DPP_LISTEN_CHANNEL_LIST "6"
#define CURVE_SEC256R1_PKEY_HEX_DIGITS     64

static const char *TAG = "wifi dapp";

static wifi_config_t s_dpp_wifi_config;

static int s_retry_num = 0;

void wifi_event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_get_config(WIFI_IF_STA, &s_dpp_wifi_config);

            if (strlen((const char *)s_dpp_wifi_config.sta.ssid) > 0) {
                // 情况B：NVS 中有 SSID 记录，尝试连接
            ESP_LOGI(TAG, "Found saved Wi-Fi SSID: %s. Trying to connect...", s_dpp_wifi_config.sta.ssid);
                esp_wifi_connect();
            } else {
            // 情况A：首次开机，直接进入配网模式
            ESP_LOGI(TAG, "No saved Wi-Fi found. Starting DPP immediately.");
            dpp_enrollee_bootstrap();
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (s_retry_num < WIFI_MAX_RETRY_NUM) {
                s_retry_num++;
                ESP_LOGI(TAG, "Disconnect event, retry to connect to the AP %d", s_retry_num);
                esp_wifi_connect();
            } else { 
               s_retry_num = 0;
            //    dpp_enrollee_bootstrap();
               esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_WIFI_DISCONNECTED, NULL, 0, portMAX_DELAY);
               ESP_LOGI(TAG, "自动连接失败启动DPP配网");
            }
            break;
        case WIFI_EVENT_STA_CONNECTED:
	        ESP_LOGI(TAG, "Successfully connected to the AP ssid : %s ", s_dpp_wifi_config.sta.ssid);
            s_retry_num = 0;
            break;
        case WIFI_EVENT_DPP_URI_READY:
            wifi_event_dpp_uri_ready_t *uri_data = event_data;
            ESP_ERROR_CHECK(esp_supp_dpp_start_listen());
            if (uri_data != NULL) {
                /* 新增：缓存 URI 数据以便外界提取 */
                strncpy(s_dpp_uri_buffer, uri_data->uri, sizeof(s_dpp_uri_buffer) - 1);
                //投递事件给 UI 层，通知二维码准备好了
                esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_QR_CODE_READY, NULL, 0 , portMAX_DELAY);
            }
            break;
        case WIFI_EVENT_DPP_CFG_RECVD:
            wifi_event_dpp_config_received_t *config = event_data;
            memcpy(&s_dpp_wifi_config, &config->wifi_cfg, sizeof(s_dpp_wifi_config));
            s_retry_num = 0;
            esp_wifi_set_config(WIFI_IF_STA, &s_dpp_wifi_config);
            esp_wifi_connect();
            break;
        case WIFI_EVENT_DPP_FAILED:
            wifi_event_dpp_failed_t *dpp_failure = event_data;
            if (s_retry_num < 5) {
                ESP_LOGI(TAG, "DPP Auth failed (Reason: %s), retry...", esp_err_to_name((int)dpp_failure->failure_reason));
                ESP_ERROR_CHECK(esp_supp_dpp_start_listen());
                s_retry_num++;
            } else {
                // xEventGroupSetBits(s_dpp_event_group, DPP_AUTH_FAIL_BIT);
                ESP_LOGI(TAG, "DPP Auth failed after max retries, giving up.");
            }

            break;
        default:
            break;
        }
    }
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        // xEventGroupSetBits(s_dpp_event_group, DPP_CONNECT_SUC_BIT);
        esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_WIFI_CONNECTED, NULL, 0, portMAX_DELAY);
    }
}

void dpp_deinit(void)
{
    esp_supp_dpp_deinit();
    s_retry_num = 0;
}

esp_err_t dpp_enrollee_bootstrap(void)
{
    ESP_ERROR_CHECK(esp_supp_dpp_init(NULL));
    esp_err_t ret;
    size_t pkey_len = strlen(DPP_BOOTSTRAPPING_KEY);
    char *key = NULL;

    if (pkey_len) {
        /* Currently only NIST P-256 curve is supported, add prefix/postfix accordingly */
        char prefix[] = "30310201010420";
        char postfix[] = "a00a06082a8648ce3d030107";

        if (pkey_len != CURVE_SEC256R1_PKEY_HEX_DIGITS) {
            ESP_LOGI(TAG, "Invalid key length! Private key needs to be 32 bytes (or 64 hex digits) long");
            return ESP_FAIL;
        }

        key = malloc(sizeof(prefix) + pkey_len + sizeof(postfix));
        if (!key) {
            ESP_LOGI(TAG, "Failed to allocate for bootstrapping key");
            return ESP_ERR_NO_MEM;
        }
        sprintf(key, "%s%s%s", prefix,DPP_BOOTSTRAPPING_KEY, postfix);
    }

    /* Currently only supported method is QR Code */
    ret = esp_supp_dpp_bootstrap_gen(DPP_LISTEN_CHANNEL_LIST, DPP_BOOTSTRAP_QR_CODE,
                                     key, DPP_DEVICE_INFO);
    if (key)
        free(key);
    
    
    return ret;
}

void wifi_init(void)
{

    //创建一个 LwIP 核心任务，并初始化 LwIP 相关工作。
    ESP_ERROR_CHECK(esp_netif_init());
    //创建默认事件循环
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    //创建默认无线局域网站点。若出现任何初始化错误，该应用程序接口将终止执行。
    esp_netif_create_default_wifi_sta();
    /* 注册 Wi-Fi 和 IP 相关事件的事件处理程序 */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    //为无线网卡驱动分配资源，包括无线网络控制结构体、接收/发送缓冲区、无线网络非易失性存储结构体等。同时，该无线网络模块会启动无线网络任务。
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_start() );
}
// 提供一个接口让 UI 层获取当前的 DPP URI（如果有的话）
const char* wifi_get_dpp_uri(void)
{
    return s_dpp_uri_buffer;
}

void time_sntp_init(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    esp_netif_sntp_init(&config);
    setenv("TZ", "CST-8", 1);
    tzset();
}
