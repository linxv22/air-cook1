#ifndef __WIFI_H__
#define __WIFI_H__

#include "esp_err.h"

void wifi_init(void);
// 获取 WiFi DPP 的 URI 字符串。如果还没准备好或配网已结束，将返回 NULL。
const char* wifi_get_dpp_uri(void);
void time_sntp_init(void);
esp_err_t dpp_enrollee_bootstrap(void);
void dpp_deinit(void);

#endif /* __WIFI_H__ */