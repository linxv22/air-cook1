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
#include "esp_heap_caps.h"
#include "esp_timer.h"

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
#include "raw_opus_encoder.h"

#include "model_path.h"

#include "my_audio.h"
#include "app_events.h"

#include "lwip/sockets.h"
#include "esp_websocket_client.h"

#include "freertos/ringbuf.h"

/* ================================================================
 *  Opus 编码参数
 * ================================================================ */
#define OPUS_SAMPLE_RATE    16000
#define OPUS_CHANNELS       1
#define OPUS_BITRATE        24000       // 24 kbps（厨房噪音场景）
#define OPUS_COMPLEXITY     0           // 最低复杂度
#define OPUS_FRAME_DURATION 20          // ms
#define OPUS_FRAME_SAMPLES  (OPUS_SAMPLE_RATE * OPUS_FRAME_DURATION / 1000)   // 320
#define OPUS_PCM_BYTES      (OPUS_FRAME_SAMPLES * sizeof(int16_t))             // 640

/* ================================================================
 *  WebSocket 统一发送队列
 * ================================================================ */
typedef struct {
    bool   is_binary;
    char  *payload;
    size_t payload_len;
    bool   free_after;
} ws_send_req_t;

static QueueHandle_t ws_send_queue = NULL;

static RingbufHandle_t g_upload_rb = NULL;

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

static audio_rec_handle_t     recorder      = NULL;
static audio_element_handle_t raw_read      = NULL;
static QueueHandle_t          rec_q         = NULL;
static volatile bool         voice_reading = false;
// 这个标志位表示当前是否正在录音中，主要用于控制 voice_read_task 的循环
static volatile bool         stop_sent     = true;

static audio_element_handle_t opus_encoder_el = NULL;

extern esp_websocket_client_handle_t client;
extern audio_element_handle_t raw_read_el;


/* ==================== WS 统一发送任务（解决锁竞争） ==================== */
static void ws_send_task(void *args)
{
    ws_send_req_t req;
    while (true) {
        if (xQueueReceive(ws_send_queue, &req, portMAX_DELAY) == pdTRUE) {
            if (req.is_binary) {
                esp_websocket_client_send_bin(client, req.payload,
                    req.payload_len, pdMS_TO_TICKS(3000));
            } else {
                esp_websocket_client_send_text(client, req.payload,
                    req.payload_len, pdMS_TO_TICKS(3000));
            }
            if (req.free_after) {
                free(req.payload);
            }
        }
    }
    vTaskDelete(NULL);
}

/* ================================================================
 *  PCM 累积 → Opus 编码 → 写入 RingBuffer
 * ================================================================ */
static bool pcm_to_opus_and_send(
    const uint8_t *pcm_data, size_t pcm_bytes,
    size_t *pcm_remain, uint8_t *pcm_buf)
{
    if (*pcm_remain + pcm_bytes > OPUS_PCM_BYTES * 2) {
        ESP_LOGE(TAG, "PCM overflow: remain=%d + new=%d", *pcm_remain, pcm_bytes);
        *pcm_remain = 0;
        return false;
    }
    memcpy(pcm_buf + *pcm_remain, pcm_data, pcm_bytes);
    *pcm_remain += pcm_bytes;

    while (*pcm_remain >= OPUS_PCM_BYTES) {
        int encoded = raw_stream_write(opus_encoder_el,
                                        (char *)pcm_buf, OPUS_PCM_BYTES);
        if (encoded < 0) {
            ESP_LOGW(TAG, "Opus encode failed: %d", encoded);
            memmove(pcm_buf, pcm_buf + OPUS_PCM_BYTES, *pcm_remain - OPUS_PCM_BYTES);
            *pcm_remain -= OPUS_PCM_BYTES;
            continue;
        }

        uint8_t opus_out[256];
        int opus_len = raw_stream_read(opus_encoder_el,
                                        (char *)opus_out, sizeof(opus_out));
        if (opus_len > 0) {
            xRingbufferSend(g_upload_rb, opus_out, opus_len, pdMS_TO_TICKS(50));
        }

        memmove(pcm_buf, pcm_buf + OPUS_PCM_BYTES, *pcm_remain - OPUS_PCM_BYTES);
        *pcm_remain -= OPUS_PCM_BYTES;
    }
    return true;
}


/* ==================== 生产者：读 PCM → Opus 编码 → RingBuffer ==================== */
static void voice_read_task(void *args)
{
    int msg = 0;
    TickType_t delay = portMAX_DELAY;
    const int buf_len = 2 * 1024;
    uint8_t *voiceData = audio_calloc(1, buf_len);

    uint8_t *pcm_acc_buf = audio_calloc(1, OPUS_PCM_BYTES * 2);
    size_t   pcm_remain  = 0;

    while (true) {
        if (xQueueReceive(rec_q, &msg, delay) == pdTRUE) {
            switch (msg) {
                case REC_START:
                    ESP_LOGW(TAG, "voice read begin");
                    delay = 0;
                    voice_reading = true;
                    pcm_remain = 0;
                    break;
                case REC_STOP:
                case REC_CANCEL:
                    ESP_LOGW(TAG, "voice read stop/cancel");
                    delay = portMAX_DELAY;
                    voice_reading = false;
                    stop_sent     = false;

                    // 刷新残余 PCM（补零编码最后一帧）
                    if (pcm_remain > 0) {
                        memset(pcm_acc_buf + pcm_remain, 0,
                               OPUS_PCM_BYTES - pcm_remain);
                        raw_stream_write(opus_encoder_el,
                                         (char *)pcm_acc_buf, OPUS_PCM_BYTES);
                        uint8_t last_opus[256];
                        int last_len = raw_stream_read(opus_encoder_el,
                                                       (char *)last_opus, sizeof(last_opus));
                        if (last_len > 0) {
                            xRingbufferSend(g_upload_rb, last_opus, last_len,
                                            pdMS_TO_TICKS(50));
                        }
                        pcm_remain = 0;
                    }
                    break;
            }
        }
        if (!voice_reading) continue;

        int len = audio_recorder_data_read(recorder, voiceData, buf_len, portMAX_DELAY);
        if (len <= 0) {
            ESP_LOGW(TAG, "recorder read finished %d", len);
            voice_reading = false;
        } else {
            pcm_to_opus_and_send(voiceData, len, &pcm_remain, pcm_acc_buf);
        }
    }
    audio_free(voiceData);
    audio_free(pcm_acc_buf);
    vTaskDelete(NULL);
}

/* ==================== 消费者：RingBuffer → ws_send_queue ==================== */
static void voice_send_task(void *args)
{
    while (true) {
        size_t len;
        char *data = (char *)xRingbufferReceive(g_upload_rb, &len,
                                                 500 / portTICK_PERIOD_MS);

        if (data == NULL && stop_sent == false) {
            ws_send_req_t req = {
                .is_binary   = false,
                .payload     = "STOP",
                .payload_len = 4,
                .free_after  = false,
            };
            xQueueSend(ws_send_queue, &req, 0);
            ESP_LOGW(TAG, "voice send stopped");
            stop_sent = true;
        }
        if (data != NULL) {
            char *copy = malloc(len);
            if (copy) {
                memcpy(copy, data, len);
                ws_send_req_t req = {
                    .is_binary   = true,
                    .payload     = copy,
                    .payload_len = len,
                    .free_after  = true,
                };
                xQueueSend(ws_send_queue, &req, 0);
            }
            vRingbufferReturnItem(g_upload_rb, data);
        }
    }
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
        if (voice_reading) {
            int msg = REC_CANCEL;
            if (xQueueSend(rec_q, &msg, 0) != pdPASS) {
                ESP_LOGE(TAG, "rec cancel send failed");
            }
        }
        esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_AUDIO_CMD, &(mic_state_t){MIC_STATE_SPEAKING}, sizeof(mic_state_t), 0);
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
        if (voice_reading) {
            int msg = REC_STOP;
            if (xQueueSend(rec_q, &msg, 0) != pdPASS) {
                ESP_LOGE(TAG, "rec stop send failed");
            }
        }
        esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_AUDIO_CMD, &(mic_state_t){MIC_STATE_LISTENING}, sizeof(mic_state_t), 0);
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
    // audio_element_set_music_info(i2s_stream_reader, 48000, 4, 16);
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
    recorder_sr_cfg.afe_cfg->vad_mode = VAD_MODE_2;
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
    raw_cfg.out_rb_size = 128 * 1024;      // 开辟 128K 的大胃口防止网络卡顿
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
    i2s_cfg.need_expand = true;           
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

    // ===== Opus 编码器 =====
    raw_opus_enc_config_t opus_enc_cfg = RAW_OPUS_ENC_CONFIG_DEFAULT();
    opus_enc_cfg.sample_rate    = OPUS_SAMPLE_RATE;
    opus_enc_cfg.channel        = OPUS_CHANNELS;
    opus_enc_cfg.bitrate        = OPUS_BITRATE;
    opus_enc_cfg.complexity     = OPUS_COMPLEXITY;
    opus_enc_cfg.frame_duration = OPUS_FRAME_DURATION;
    opus_encoder_el = raw_opus_encoder_init(&opus_enc_cfg);
    if (opus_encoder_el == NULL) {
        ESP_LOGE(TAG, "Opus encoder init failed");
        return;
    }
    ESP_LOGI(TAG, "Opus encoder ready: %dHz, %dch, %dbps",
             OPUS_SAMPLE_RATE, OPUS_CHANNELS, OPUS_BITRATE);

    // ===== WS 统一发送队列 + 任务 =====
    ws_send_queue = xQueueCreate(16, sizeof(ws_send_req_t));
    audio_thread_create(NULL, "ws_send", ws_send_task, NULL,
                        4 * 1024, 4, true, 0);
    
    rec_q = xQueueCreate(3, sizeof(int));
   
    g_upload_rb = xRingbufferCreate(256 * 1024 * 2, RINGBUF_TYPE_BYTEBUF);
    if (!g_upload_rb) {
        ESP_LOGE(TAG, "Upload ring buffer create failed");
        return;
    }

    es7210_adc_set_volume(GAIN_30DB);
    start_recorder();

    audio_thread_create(NULL, "read_task", voice_read_task, NULL, 6 * 1024, 5, true, 0);
    audio_thread_create(NULL, "send_task", voice_send_task, NULL, 4 * 1024, 3, true, 1);


}
