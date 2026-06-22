# 智能空气炸锅 ESP32-S3 接口文档

> **适用对象**：ESP32-S3 固件开发者  
> **通信架构**：ESP32-S3 ←→ Python 云服务器 ←→ 手机 APP  
> **说明**：ESP32-S3 不直接与 APP 通信，所有数据通过云服务器中转。  
> **JSON 规范**：所有消息统一使用 `action` 字段区分类型，`funSpeed` 用 int（0=高, 1=中, 2=低）。

---

## 1. 统一 JSON 格式速查

### ESP32 → 服务器（仅 1 种）

| action 值 | 频率 | 说明 |
|-----------|------|------|
| `"status"` | running 每5s / idle 每30s / stopped连续2次每5s | 温度状态上报 |

### 服务器 → ESP32（共 7 种）

| action 值 | 触发条件 | 说明 |
|-----------|---------|------|
| `"welcome"` | ESP32 建立 WebSocket 连接时 | 欢迎消息 + TTS 语音推荐 |
| `"cook"` | APP点开始 / 语音说"帮我炸鸡翅" | 开始烹饪 |
| `"start"` | 用户说"开始加热" / APP确认 | 确认开始加热 |
| `"stop"` | APP点停止 / 倒计时结束 | 停止烹饪 |
| `"pause"` | 用户说"暂停" | 暂停烹饪 |
| `"schedule"` | 预约时间到达 | 预约烹饪开始 |
| `"chat"` | 用户问天气/推荐等闲聊 | 语音回复 |

---

## 2. 设备连接

### 2.1 连接信息

| 项目 | 值 |
|------|-----|
| 服务器地址 | `ws://8.162.21.140:8765` |
| 连接时机 | ESP32 联网成功后立即建立并保持长连接 |
| 重连策略 | 断线后自动重连，间隔 5 秒，无限重试 |

### 2.2 在线判定

ESP32 **不需要发送心跳包**。服务器通过 `action: "status"` 的接收时间判定在线：

| 判定规则 | 离线条件 |
|----------|---------|
| 收到 `action: "status"` | 刷新在线状态 |
| 超过 **60 秒** 未收到任何 status 数据 | 判定为离线 |

> 因此 ESP32 只要按规定的频率上报 status，服务器就能自动知道设备在线。

### 2.3 APP 绑定

APP 登录后用户输入服务器 IP:端口完成绑定。ESP32 **无需显示二维码**，**无需维护 `device_token`**。绑定过程 ESP32 完全不感知。

---

## 3. ESP32 上报数据（ESP32 → 服务器）

### 3.1 上报频率

| 设备状态 | 上报频率 | 说明 |
|----------|---------|------|
| `running`（运行中） | **每 5 秒** | 烹饪进行中，需实时监控温度 |
| `stopped`（已停止） | **每 5 秒，连续 2 次** | 确保服务器收到停止通知 |
| `idle`（空闲） | **每 30 秒** | 待机状态，低频上报即可 |

### 3.2 状态上报 JSON（唯一格式）

```json
{"action":"status","temp":185.5,"target_temp":200,"time_left":415,"state":"running","food_name":"鸡翅","funSpeed":0}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `action` | string | 固定 `"status"` |
| `temp` | float | 当前炉内温度（摄氏度），保留 1 位小数 |
| `target_temp` | int | 目标温度，running 时发送，idle/stopped 时为 0 |
| `time_left` | int | 剩余烹饪时间（秒），stopped/idle 时为 0 |
| `state` | string | `"running"` / `"stopped"` / `"idle"` |
| `food_name` | string | 当前食物名称，idle 时为空 `""` |
| `funSpeed` | int | `0`=高, `1`=中, `2`=低 |

### 3.3 各状态示例

**运行中（每 5 秒）**：
```json
{"action":"status","temp":185.5,"target_temp":200,"time_left":415,"state":"running","food_name":"鸡翅","funSpeed":0}
```

**空闲（每 30 秒）**：
```json
{"action":"status","temp":26.0,"target_temp":0,"time_left":0,"state":"idle","food_name":"","funSpeed":2}
```

**停止后（连续 2 次，间隔 5 秒，之后切回空闲 30 秒）**：
```json
{"action":"status","temp":195.0,"target_temp":200,"time_left":0,"state":"stopped","food_name":"鸡翅","funSpeed":2}
```

---

## 4. ESP32 接收指令（服务器 → ESP32）

ESP32 统一解析 `action` 字段做路由，所有指令附带 `reply` 字段（可直接送 TTS 播报 / LCD 显示）。

### 4.1 欢迎消息

```
触发：ESP32 建立 WebSocket 连接时服务器自动发送
```

```json
{"action":"welcome","reply":"欢迎使用智能空气炸锅！已为您成功加载5条历史记录，根据您的历史记录，为您推荐鸡腿..."}
```

**ESP32 收到后**：播放 `reply` TTS 语音 + LCD 显示欢迎文字。

### 4.2 开始烹饪

```
触发：手机APP点开始 / 用户对炸锅说"帮我炸鸡翅"
```

```json
{"action":"cook","food":"鸡翅","temp":180,"time":300,"funSpeed":0,"reply":"好的，设定温度180摄氏度，风扇转速高，您的鸡翅预计5分钟内炸好！"}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `action` | string | 固定 `"cook"` |
| `food` | string | 食物名称 |
| `temp` | int | 目标温度（摄氏度），范围 40-220 |
| `time` | int | 烹饪时长（秒），范围 60-3600 |
| `funSpeed` | int | `0`=高, `1`=中, `2`=低 |
| `reply` | string | TTS 播报 / LCD 显示文本 |

**ESP32 收到后应做**：
1. 保存食物名和参数
2. 启动加热管，PID 控温到目标温度
3. 启动风扇到指定转速
4. 开始倒计时，**每 5 秒**上报 `action: "status"`（state=`"running"`）
5. 倒计时归零后自动停止加热，state 变 `"stopped"`，连续上报 2 次后切回空闲频率
6. 停止后继续保持 WebSocket 连接，等待下次指令

### 4.3 确认加热

```
触发：用户对炸锅说"开始加热" / APP 确认
```

```json
{"action":"start","reply":"好的，空气炸锅现在开始加热工作，祝您用餐愉快！"}
```

**ESP32 收到后**：如果处于待确认状态则开始加热，否则播放 reply 语音即可。

### 4.4 停止烹饪

```
触发：手机APP点停止 / 烹饪倒计时结束
```

```json
{"action":"stop","reply":"已停止烹饪"}
```

**ESP32 收到后应做**：
1. 立即停止加热管
2. 风扇继续运行 30 秒散热，然后停止
3. state 变为 `"stopped"`
4. 以 `"stopped"` 状态上报 2 次（间隔 5 秒）
5. 之后切换为空闲上报频率（每 30 秒）

### 4.5 暂停烹饪

```
触发：用户对炸锅说"暂停"
```

```json
{"action":"pause","reply":"好的，已为您暂停空气炸锅！"}
```

**ESP32 收到后应做**：暂停加热和倒计时，保留当前进度，等待恢复指令。

### 4.6 预约烹饪

```
触发：预约时间到达时服务器自动下发
```

```json
{"action":"schedule","food":"牛排","temp":200,"time":480,"funSpeed":0,"scheduled_at":"2026-11-26-00:00:00","reply":"已为你预约好在2026年11月26日0点0分烹饪牛排..."}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `action` | string | 固定 `"schedule"` |
| `food` | string | 食物名称 |
| `temp` | int | 目标温度（摄氏度） |
| `time` | int | 烹饪时长（秒） |
| `funSpeed` | int | `0`=高, `1`=中, `2`=低 |
| `scheduled_at` | string | 预约执行时间 `"YYYY-MM-DD-HH:mm:ss"` |
| `reply` | string | TTS 播报文本 |

**ESP32 收到后应做**：
1. 保存预约参数到本地
2. 在 `scheduled_at` 指定时间自动开始烹饪
3. 开始烹饪后按 running 状态每 5 秒上报
4. 播放 reply 语音确认

> 📌 服务器会在预约时间到达时下发此指令。为防止网络延迟，服务器会提前 2 秒下发，ESP32 收到后等待到精确时间再启动。

### 4.7 闲聊回复

```
触发：用户问天气/推荐等非烹饪对话
```

```json
{"action":"chat","reply":"今天天气是晴天，气温22到29度，不建议您吃太多油炸食品！"}
```

**ESP32 收到后**：播放 `reply` TTS 语音 + LCD 显示回复文字。

---

## 5. ESP32 固件伪代码

```c
// 统一解析 action 字段做路由
void handle_server_message(const char* json_str) {
    cJSON* root = cJSON_Parse(json_str);
    char* action = cJSON_GetStringValue(cJSON_GetObjectItem(root, "action"));
    char* reply = cJSON_GetStringValue(cJSON_GetObjectItem(root, "reply"));

    if (reply) tts_play(reply);           // 所有指令都附带语音
    if (reply) lcd_show(reply);           // 同时显示在 LCD 上

    if (strcmp(action, "welcome") == 0) {
        // 仅播报语音，无操作
    }
    else if (strcmp(action, "cook") == 0) {
        char* food = cJSON_GetStringValue(cJSON_GetObjectItem(root, "food"));
        int temp = cJSON_GetIntValue(cJSON_GetObjectItem(root, "temp"));
        int time = cJSON_GetIntValue(cJSON_GetObjectItem(root, "time"));
        int funSpeed = cJSON_GetIntValue(cJSON_GetObjectItem(root, "funSpeed"));
        start_cooking(food, temp, time, funSpeed);
    }
    else if (strcmp(action, "start") == 0) {
        confirm_start();
    }
    else if (strcmp(action, "stop") == 0) {
        stop_cooking();
    }
    else if (strcmp(action, "pause") == 0) {
        pause_cooking();
    }
    else if (strcmp(action, "schedule") == 0) {
        char* food = cJSON_GetStringValue(cJSON_GetObjectItem(root, "food"));
        int temp = cJSON_GetIntValue(cJSON_GetObjectItem(root, "temp"));
        int time = cJSON_GetIntValue(cJSON_GetObjectItem(root, "time"));
        int funSpeed = cJSON_GetIntValue(cJSON_GetObjectItem(root, "funSpeed"));
        char* scheduled_at = cJSON_GetStringValue(cJSON_GetObjectItem(root, "scheduled_at"));
        schedule_cooking(food, temp, time, funSpeed, scheduled_at);
    }
    else if (strcmp(action, "chat") == 0) {
        // 仅播报语音，无操作
    }

    cJSON_Delete(root);
}
```

---

## 6. 完整通信流程

### 6.1 APP 控制烹饪流程

```
ESP32                         服务器                      APP
  │                              |                          |
  │--- ws连接 ────────────────>|                          |
  │   <── welcome + TTS ────────|                          |
  │--- status (idle, 30s) ────>|                          |
  │                              |                          |
  │                              |   <── 用户输入IP:端口绑定 |
  │                              |   <── POST /api/cook     |
  │                              |                          |
  │   <── cook + TTS确认 ────────|                          |
  │       "要现在加热吗？"        |                          |
  │                              |                          |
  │   用户说"开始加热"            |                          |
  │--- PCM音频流 ──────────────>|                          |
  │   <── start ─────────────────|                          |
  │                              |                          |
  │--- status (running, 5s) ───>|                          |
  │--- status (running, 5s) ───>|-- GET /api/status (2s) ->|
  │        ...                   |         ...              |
  │--- status (stopped, 5s) ───>|                          |
  │--- status (stopped, 5s) ───>|                          |
  │--- status (idle, 30s) ─────>|                          |
```

### 6.2 预约烹饪流程

```
APP                         服务器                     ESP32
 |                             |                          |
 |-- POST /api/schedule ────>|                          |
 |   <── {success:true} ──────|                          |
 |                             |                          |
 |                             |   <── TTS预约确认语音 ──→|
 |                             |                          |
 |                             |   ...(等待预约时间)...    |
 |                             |                          |
 |                             |-- 定时器触发(提前2秒) ──→|
 |                             |   schedule指令 + TTS ──→|
 |                             |                          |
 |                             |   <── status (running) ──| (每5秒)
 |                             |                          |
 |   <── 每2秒轮询状态 ──────────|                          |
```

---

## 7. 上报频率速查

```
┌──────────┬──────────────────────────────────────────────┐
│   状态   │           上报间隔                            │
├──────────┼──────────────────────────────────────────────┤
│  running │  ██ 5秒  ██  (温度监控需频繁)                 │
│  stopped │  ██ 5秒  ██  (仅连续2次, 之后切idle)          │
│  idle    │  ██ 30秒 ██  (待机省带宽)                    │
└──────────┴──────────────────────────────────────────────┘

在线判定: 服务器 60 秒内收到 status → 在线
          超过 60 秒未收到    → 离线
```

---

## 8. funSpeed 对照表

| funSpeed 值 | 含义 | 风扇转速 |
|-------------|------|---------|
| `0` | 高 | High |
| `1` | 中 | Medium |
| `2` | 低 | Low |

---

## 9. 错误处理

| 场景 | ESP32 处理方式 |
|------|---------------|
| WebSocket 断线 | 自动重连，间隔 5 秒递增（最大 60 秒） |
| 烹饪中收到新 cook 指令 | 停止当前烹饪，执行新指令 |
| 温度传感器故障 | state 变为 `"stopped"`，上报停止状态 |
| 温度超过 250°C（过热保护） | 立即停止加热，state 变为 `"stopped"` |
| 网络长时间断线（>5分钟） | 按本地参数继续完成烹饪（不依赖服务器） |
| 收到未知 action | 忽略，保持当前状态 |

---

## 10. ESP32 启动完整流程

```
1. 上电
2. 初始化 LCD、温度传感器、WiFi
3. 如果是首次启动（无 WiFi 凭据）：
   a. LCD 显示 WiFi 配网二维码（用户扫码输入 WiFi 密码）
   b. 配网成功，保存凭据到 NVS
4. 连接 WiFi
5. LCD 显示「已联网」及本机 IP 地址
6. 建立 WebSocket 连接到 ws://8.162.21.140:8765
7. 接收 welcome 消息 → 播放欢迎 TTS + LCD 显示
8. 发送首次 status 上报（state: "idle"）
9. 进入主循环：
   - 监听 WebSocket，解析 action 字段路由（cook/start/stop/pause/schedule/chat）
   - 运行中每 5 秒上报 status
   - 空闲时每 30 秒上报 status
   - 更新 LCD 显示（温度、时间、状态、风扇转速）
   - 无需发送心跳包（status 即心跳）
```
