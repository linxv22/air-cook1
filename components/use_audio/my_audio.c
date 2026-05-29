/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2024 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "amrnb_encoder.h"
#include "amrwb_encoder.h"
#include "audio_element.h"
#include "audio_idf_version.h"
#include "audio_mem.h"
#include "audio_pipeline.h"
#include "audio_recorder.h"
#include "audio_thread.h"

#include "board.h"
#include "esp_audio.h"
#include "filter_resample.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "raw_stream.h"
#include "recorder_encoder.h"
#include "recorder_sr.h"
#include "tone_stream.h"
#include "es7210.h"

#include "model_path.h"

#include "my_audio.h"
#include "app_events.h"

#include "lwip/sockets.h"
#include "esp_websocket_client.h"

extern const uint8_t lr_pcm_start[] asm("_binary_dingdong_raw_start");
extern const uint8_t lr_pcm_end[]   asm("_binary_dingdong_raw_end");

#define WAKENET_ENABLE      (true)
#define MULTINET_ENABLE     (true)

enum _rec_msg_id {
    REC_START = 1,
    REC_STOP,
    REC_CANCEL,
};

static char *TAG = "wwe_example";

esp_audio_handle_t     player_handle = NULL;
static audio_rec_handle_t     recorder      = NULL;
static audio_element_handle_t raw_read      = NULL;
static QueueHandle_t          rec_q         = NULL;
static bool                   voice_reading = false;

extern esp_websocket_client_handle_t client;
extern audio_element_handle_t raw_read_el;

static void voice_read_task(void *args)
{
    const int buf_len = 2 * 1024;
    char *voiceData = audio_calloc(1, buf_len);
    int msg = 0;
    TickType_t delay = portMAX_DELAY;    

    while (true) {
        if (xQueueReceive(rec_q, &msg, delay) == pdTRUE) {
            switch (msg) {
                case REC_START: {
                    ESP_LOGW(TAG, "voice read begin");
                    delay = 0;
                    voice_reading = true;

                    break;
                }
                case REC_STOP: {
                    ESP_LOGW(TAG, "voice read stopped");
                    delay = portMAX_DELAY;
                    voice_reading = false;
                    break;
                }
                case REC_CANCEL: {
                    ESP_LOGW(TAG, "voice read cancel");
                    delay = portMAX_DELAY;
                    voice_reading = false;
                    break;
                }
                default:
                    break;
            }
        }
        int ret = 0;
        if (voice_reading) {
            ret = audio_recorder_data_read(recorder, voiceData, buf_len, portMAX_DELAY);
            if (ret <= 0) {
                ESP_LOGW(TAG, "audio recorder read finished %d", ret);
                delay = portMAX_DELAY;
                voice_reading = false;
            }
            else {
                //发送音频数据到服务器
                esp_websocket_client_send_bin(client,(const char *)voiceData,ret,portMAX_DELAY);
                // printf("[WebSocket] 已发送 %d 字节的音频数据\n", ret);
            }
        }
    }

    free(voiceData);
    vTaskDelete(NULL);
}

static esp_err_t rec_engine_cb(audio_rec_evt_t *event, void *user_data)
{
    if (AUDIO_REC_WAKEUP_START == event->type) {
        recorder_sr_wakeup_result_t *wakeup_result = event->event_data;

        ESP_LOGI(TAG, "rec_engine_cb - REC_EVENT_WAKEUP_START");
        ESP_LOGI(TAG, "wakeup: vol %f, mod idx %d, word idx %d", wakeup_result->data_volume, wakeup_result->wakenet_model_index, wakeup_result->wake_word_index);
        size_t dingdong_len = lr_pcm_end - lr_pcm_start;
        if (raw_read_el != NULL) {
            raw_stream_write(raw_read_el, (char *)lr_pcm_start, dingdong_len);
        }
        esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_AUDIO_CMD, &(mic_state_t){MIC_STATE_SPEAKING}, sizeof(mic_state_t), portMAX_DELAY);
        if (voice_reading) {
            int msg = REC_CANCEL;
            if (xQueueSend(rec_q, &msg, 0) != pdPASS) {
                ESP_LOGE(TAG, "rec cancel send failed");
            }
        }
    } else if (AUDIO_REC_VAD_START == event->type) {
        ESP_LOGI(TAG, "rec_engine_cb - REC_EVENT_VAD_START");
        if (!voice_reading) {
            int msg = REC_START;
            if (xQueueSend(rec_q, &msg, 0) != pdPASS) {
                ESP_LOGE(TAG, "rec start send failed");
            }
        }
    } else if (AUDIO_REC_VAD_END == event->type) {
        ESP_LOGI(TAG, "rec_engine_cb - REC_EVENT_VAD_STOP");
        esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_AUDIO_CMD, &(mic_state_t){MIC_STATE_LISTENING}, sizeof(mic_state_t), portMAX_DELAY);
        if (voice_reading) {
            int msg = REC_STOP;
            if (xQueueSend(rec_q, &msg, 0) != pdPASS) {
                ESP_LOGE(TAG, "rec stop send failed");
            }
        }

    } else if (AUDIO_REC_WAKEUP_END == event->type) {
        ESP_LOGI(TAG, "rec_engine_cb - REC_EVENT_WAKEUP_END");
        AUDIO_MEM_SHOW(TAG);
    } 
    else {
        ESP_LOGE(TAG, "Unkown event");
    }
    return ESP_OK;
}

static int input_cb_for_afe(int16_t *buffer, int buf_sz, void *user_ctx, TickType_t ticks)
{
    return raw_stream_read(raw_read, (char *)buffer, buf_sz);
}

static void start_recorder()
{
    char *audio_sr_input_fmt = AUDIO_ADC_INPUT_CH_FORMAT;
    audio_element_handle_t i2s_stream_reader;
    audio_pipeline_handle_t pipeline;
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    if (NULL == pipeline) {
        return;
    }
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(CODEC_ADC_I2S_PORT, 48000, 32, AUDIO_STREAM_READER);
    i2s_stream_reader = i2s_stream_init(&i2s_cfg);
    audio_element_set_music_info(i2s_stream_reader, 48000, 2, 16);
    audio_element_handle_t filter = NULL;
    rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    rsp_cfg.src_rate = 48000;
    rsp_cfg.dest_rate = 16000;
    rsp_cfg.mode = RESAMPLE_UNCROSS_MODE;
    rsp_cfg.src_ch = 4;
    rsp_cfg.dest_ch = 4;
    rsp_cfg.max_indata_bytes = 1024;
    filter = rsp_filter_init(&rsp_cfg);

    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_READER;
    raw_read = raw_stream_init(&raw_cfg);

    audio_pipeline_register(pipeline, i2s_stream_reader, "i2s");
    audio_pipeline_register(pipeline, raw_read, "raw");

    if (filter) {
        audio_pipeline_register(pipeline, filter, "filter");
        const char *link_tag[3] = {"i2s", "filter", "raw"};
        audio_pipeline_link(pipeline, &link_tag[0], 3);
        esp_log_level_set("RSP_FILTER", ESP_LOG_INFO);
    } else {
        const char *link_tag[2] = {"i2s", "raw"};
        audio_pipeline_link(pipeline, &link_tag[0], 2);
    }

    audio_pipeline_run(pipeline);
    ESP_LOGI(TAG, "Recorder has been created");

    recorder_sr_cfg_t recorder_sr_cfg = DEFAULT_RECORDER_SR_CFG(audio_sr_input_fmt, "model", AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    recorder_sr_cfg.afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    recorder_sr_cfg.afe_cfg->wakenet_init = WAKENET_ENABLE;
    recorder_sr_cfg.afe_cfg->vad_mode = VAD_MODE_4;
    recorder_sr_cfg.multinet_init = 0;
#if !defined(CONFIG_SR_MN_CN_NONE)
    recorder_sr_cfg.mn_language = ESP_MN_CHINESE;
#elif !defined(CONFIG_SR_MN_EN_NONE)
    recorder_sr_cfg.mn_language = ESP_MN_ENGLISH;
#else
    // recorder_sr_cfg.mn_language = "";
#endif
    recorder_sr_cfg.afe_cfg->aec_init = true;

    recorder_sr_cfg.afe_cfg->agc_init = true;
    recorder_sr_cfg.afe_cfg->agc_mode = AFE_AGC_MODE_WAKENET;
    recorder_sr_cfg.afe_cfg->afe_ns_mode = AFE_NS_MODE_WEBRTC;

    audio_rec_cfg_t cfg = AUDIO_RECORDER_DEFAULT_CFG();
    cfg.read = (recorder_data_read_t)&input_cb_for_afe;
    cfg.sr_handle = recorder_sr_create(&recorder_sr_cfg, &cfg.sr_iface);
    cfg.event_cb = rec_engine_cb;
    cfg.vad_off = 1500;
    cfg.vad_start = 300;
    recorder = audio_recorder_create(&cfg);
}

static void log_clear(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("AUDIO_THREAD", ESP_LOG_ERROR);
    esp_log_level_set("I2C_BUS", ESP_LOG_ERROR);
    esp_log_level_set("AUDIO_HAL", ESP_LOG_ERROR);
    esp_log_level_set("I2S", ESP_LOG_ERROR);
    esp_log_level_set("AUDIO_PIPELINE", ESP_LOG_ERROR);
    esp_log_level_set("AUDIO_ELEMENT", ESP_LOG_ERROR);
    esp_log_level_set("I2S_STREAM", ESP_LOG_ERROR);
    esp_log_level_set("RSP_FILTER", ESP_LOG_ERROR);
    esp_log_level_set("AUDIO_EVT", ESP_LOG_ERROR);
    // 【已删】删除了音频播放相关的日志设置
}

void my_audio_init(void)
{
    log_clear();
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t i2s_stream_writer;
    audio_board_handle_t board_handle = audio_board_init();
    

    // ----------- 1. 配置硬件为双工模式 (BOTH) -----------
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);
    audio_hal_set_volume(board_handle->audio_hal, 100);
    
    // ----------- 3. 建立你的独立播放流水线 -----------
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);

    // [第一节管道]：RAW 输入池 (从 WebSocket 接收数据)
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_READER;   // 作为管线的源头
    raw_cfg.out_rb_size = 128 * 1024;      // 开辟 64K 的大胃口防止网络卡顿
    raw_read_el = raw_stream_init(&raw_cfg);

    // [第二节管道]：升频转换器 (16000Hz 转 48000Hz)
    rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    rsp_cfg.src_rate = 16000;             // WebSocket 送来的是 16K
    rsp_cfg.src_ch = 1;
    rsp_cfg.src_bits = 16;
    rsp_cfg.dest_rate = 48000;            // 底层硬件要求 48K（必须和录音保持一致！）
    rsp_cfg.dest_ch = 2;
    rsp_cfg.dest_bits = 16;
    audio_element_handle_t filter_el = rsp_filter_init(&rsp_cfg);

    // [第三节管道]：I2S 硬件输出 
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(CODEC_ADC_I2S_PORT, 48000, 32, AUDIO_STREAM_WRITER);
    i2s_cfg.need_expand = true;           // 【救命神键】：将 16位 音频自动拉伸到 32位，完美匹配底层硬件！
    i2s_cfg.expand_src_bits = 16;
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);

    // ----------- 4. 注册与链接 -----------
    audio_pipeline_register(pipeline, raw_read_el, "raw");
    audio_pipeline_register(pipeline, filter_el, "filter");
    audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

    // 把它们串起来：raw -> filter -> i2s
    const char *link_tag[3] = {"raw", "filter", "i2s"};
    audio_pipeline_link(pipeline, &link_tag[0], 3);

    // ----------- 5. 运行流水线 -----------
    // 启动后它会自动停在此处挂起并侦听 raw_read_el，不占用 CPU，等待数据降临！
    audio_pipeline_run(pipeline);
    
    es7210_adc_set_volume(GAIN_37_5DB);
    start_recorder();
    
    rec_q = xQueueCreate(3, sizeof(int));
    audio_thread_create(NULL, "read_task", voice_read_task, NULL, 4 * 1024, 5, true, 0);
   
}
