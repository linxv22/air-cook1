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
#include "app_events.h"


#include "lwip/err.h"
#include "lwip/sys.h"

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_dpp_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */

#define DPP_CONNECTED_BIT        BIT0
#define DPP_CONNECT_FAIL_BIT     BIT1
#define DPP_AUTH_FAIL_BIT        BIT2
#define WIFI_MAX_RETRY_NUM          5

#define DPP_DEVICE_INFO ""
#define DPP_BOOTSTRAPPING_KEY ""
#define DPP_LISTEN_CHANNEL_LIST "6"
#define CURVE_SEC256R1_PKEY_HEX_DIGITS     64


static const char *TAG = "wifi dapp";


wifi_config_t s_dpp_wifi_config;

static int s_retry_num = 0;


static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_ERROR_CHECK(esp_supp_dpp_start_listen());
            ESP_LOGI(TAG, "Started listening for DPP Authentication");
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (s_retry_num < WIFI_MAX_RETRY_NUM) {
                esp_wifi_connect();
                s_retry_num++;
                ESP_LOGI(TAG, "Disconnect event, retry to connect to the AP");
            } else {
                xEventGroupSetBits(s_dpp_event_group, DPP_CONNECT_FAIL_BIT);
            }
            break;
        case WIFI_EVENT_STA_CONNECTED:
	    ESP_LOGI(TAG, "Successfully connected to the AP ssid : %s ", s_dpp_wifi_config.sta.ssid);
            break;
        case WIFI_EVENT_DPP_URI_READY:
            wifi_event_dpp_uri_ready_t *uri_data = event_data;
            if (uri_data != NULL) {
                esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();

                ESP_LOGI(TAG, "Scan below QR Code to configure the enrollee:");
                //控制台显示二维码
                // esp_qrcode_generate(&cfg, (const char *)uri_data->uri);
                //投递事件给 UI 层，携带 URI 数据
                esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_QR_CODE_READY, uri_data->uri, uri_data->uri_data_len, portMAX_DELAY);
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
                xEventGroupSetBits(s_dpp_event_group, DPP_AUTH_FAIL_BIT);
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
        xEventGroupSetBits(s_dpp_event_group, DPP_CONNECTED_BIT);
        esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_WIFI_CONNECTED, NULL, 0, portMAX_DELAY);
    }
}


esp_err_t dpp_enrollee_bootstrap(void)
{
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

    s_dpp_event_group = xEventGroupCreate();
    //初始化底层TCP/IP协议栈
    ESP_ERROR_CHECK(esp_netif_init());
    //创建默认事件循环
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    //创建默认无线局域网站点。若出现任何初始化错误，该应用程序接口将终止执行。
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    //为无线网卡驱动分配资源，包括无线网络控制结构体、接收/发送缓冲区、无线网络非易失性存储结构体等。同时，该无线网络模块会启动无线网络任务。
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* 注册 Wi-Fi 和 IP 相关事件的事件处理程序 */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));


    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_supp_dpp_init());
    ESP_ERROR_CHECK(dpp_enrollee_bootstrap());
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_dap finished.");    

  /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by wifi_event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_dpp_event_group,
                                           DPP_CONNECTED_BIT | DPP_CONNECT_FAIL_BIT | DPP_AUTH_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & DPP_CONNECTED_BIT) {
    } else if (bits & DPP_CONNECT_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 s_dpp_wifi_config.sta.ssid, s_dpp_wifi_config.sta.password);
    } else if (bits & DPP_AUTH_FAIL_BIT) {
        ESP_LOGI(TAG, "DPP Authentication failed after %d retries", s_retry_num);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    esp_supp_dpp_deinit();
    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler));
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler));
    vEventGroupDelete(s_dpp_event_group);


}

