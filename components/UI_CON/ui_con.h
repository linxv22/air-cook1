#ifndef UI_CON_H
#define UI_CON_H

#pragma once
#include "app_events.h"
#include "WIFI.h"
// UI 控制模块，负责所有与界面相关的逻辑，包括界面布局、事件响应、状态显示等
void ui_start(void);

void ui_wifi_up(WIFI_state_t state);
void ui_up_temp(float temp, int rem_time_s);
void ui_mic_state_update(mic_state_t state);
void ui_show_cooking_complete(void);
void ui_show_cloud_detail(cloud_data_t *data);
void ui_cloud_start(void);

#endif // ui_con.h
