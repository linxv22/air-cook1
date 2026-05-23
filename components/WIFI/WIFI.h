#ifndef __WIFI_H__
#define __WIFI_H__

void wifi_init(void);
// 获取 WiFi DPP 的 URI 字符串。如果还没准备好或配网已结束，将返回 NULL。
const char* wifi_get_dpp_uri(void);

#endif /* __WIFI_H__ */