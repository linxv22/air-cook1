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
#include "periph_adc_button.h"
#include "raw_stream.h"
#include "recorder_encoder.h"
#include "recorder_sr.h"
#include "tone_stream.h"
#include "es7210.h"

#include "model_path.h"

#include "my_audio.h"

#include "lwip/sockets.h"
#include "esp_websocket_client.h"

// static const char *TAG = "my_audio";

// static struct marker {
//     int pos;
//     const uint8_t *start;
//     const uint8_t *end;
// } file_marker;

// // 嵌入的三个不同采样率的 MP3 文件
// extern const uint8_t lr_mp3_start[] asm("_binary_music_16b_2c_8000hz_mp3_start");
// extern const uint8_t lr_mp3_end[]   asm("_binary_music_16b_2c_8000hz_mp3_end");

// extern const uint8_t mr_mp3_start[] asm("_binary_music_16b_2c_22050hz_mp3_start");
// extern const uint8_t mr_mp3_end[]   asm("_binary_music_16b_2c_22050hz_mp3_end");

// extern const uint8_t hr_mp3_start[] asm("_binary_music_16b_2c_44100hz_mp3_start");
// extern const uint8_t hr_mp3_end[]   asm("_binary_music_16b_2c_44100hz_mp3_end");

// static void set_next_file_marker(void)
// {
//     static int idx = 0;

//     switch (idx) {
//         case 0:
//             file_marker.start = lr_mp3_start;
//             file_marker.end   = lr_mp3_end;
//             ESP_LOGI(TAG, "[ * ] Switch to: 8000Hz MP3");
//             break;
//         case 1:
//             file_marker.start = mr_mp3_start;
//             file_marker.end   = mr_mp3_end;
//             ESP_LOGI(TAG, "[ * ] Switch to: 22050Hz MP3");
//             break;
//         case 2:
//             file_marker.start = hr_mp3_start;
//             file_marker.end   = hr_mp3_end;
//             ESP_LOGI(TAG, "[ * ] Switch to: 44100Hz MP3");
//             break;
//         default:
//             ESP_LOGE(TAG, "[ * ] Unsupported index = %d", idx);
//     }
//     if (++idx > 2) {
//         idx = 0;
//     }
//     file_marker.pos = 0;
// }

// int mp3_music_read_cb(audio_element_handle_t el, char *buf, int len, TickType_t wait_time, void *ctx)
// {
//     int read_size = file_marker.end - file_marker.start - file_marker.pos;
//     if (read_size == 0) {
//         return AEL_IO_DONE;
//     } else if (len < read_size) {
//         read_size = len;
//     }
//     memcpy(buf, file_marker.start + file_marker.pos, read_size);
//     file_marker.pos += read_size;
//     return read_size;
// }

// void my_audio_init(void)
// {
//     audio_pipeline_handle_t pipeline;
//     audio_element_handle_t i2s_stream_writer, mp3_decoder;

//     esp_log_level_set("*", ESP_LOG_WARN);
//     esp_log_level_set(TAG, ESP_LOG_INFO);

//     // ---------------- [ 1 ] 初始化音频板及 Codec 芯片 ----------------
//     ESP_LOGI(TAG, "[ 1 ] Start audio codec chip");
//     audio_board_handle_t board_handle = audio_board_init();
//     audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

//     int player_volume = 100; 
//     audio_hal_set_volume(board_handle->audio_hal, player_volume);

//     // ---------------- [ 2 ] 创建并初始化管道 ----------------
//     ESP_LOGI(TAG, "[ 2 ] Create audio pipeline");
//     audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
//     pipeline = audio_pipeline_init(&pipeline_cfg);
//     mem_assert(pipeline);

//     // ---------------- [ 2.1 ] 创建 MP3 解码器 ----------------
//     ESP_LOGI(TAG, "[2.1] Create mp3 decoder");
//     mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
//     mp3_decoder = mp3_decoder_init(&mp3_cfg);
//     audio_element_set_read_cb(mp3_decoder, mp3_music_read_cb, NULL);

//     // ---------------- [ 2.2 ] 创建 I2S 输出流 ----------------
//     ESP_LOGI(TAG, "[2.2] Create i2s stream");
    
// #if defined CONFIG_ESP32_C3_LYRA_V2_BOARD
//     i2s_stream_cfg_t i2s_cfg = I2S_STREAM_PDM_TX_CFG_DEFAULT();
// #else
//     i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
// #endif

   

//     i2s_stream_writer = i2s_stream_init(&i2s_cfg);

//     // ---------------- [ 2.3 ] 注册与链接 ----------------
//     ESP_LOGI(TAG, "[2.3] Register elements to pipeline");
//     audio_pipeline_register(pipeline, mp3_decoder, "mp3");
//     audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

//     ESP_LOGI(TAG, "[2.4] Link elements");
//     const char *link_tag[2] = {"mp3", "i2s"};
//     audio_pipeline_link(pipeline, &link_tag[0], 2);

//     // ---------------- [ 4 ] 配置事件监听器 ----------------
//     ESP_LOGI(TAG, "[ 4 ] Set up event listener");
//     audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
//     audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

//     ESP_LOGI(TAG, "[4.1] Listening event from pipeline");
//     audio_pipeline_set_listener(pipeline, evt);

//     // ---------------- [ 5 ] 启动音频播放 ----------------
//     ESP_LOGI(TAG, "[ 5 ] Start audio_pipeline");
//     set_next_file_marker(); 
//     audio_pipeline_run(pipeline);

//     while (1) {
//         audio_event_iface_msg_t msg;
//         esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
//         if (ret != ESP_OK) {
//             continue;
//         }

//         // 格式上报事件：根据解码出的格式更新 I2S 时钟
//         if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) mp3_decoder
//             && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
//             audio_element_info_t music_info = {0};
//             audio_element_getinfo(mp3_decoder, &music_info);
            
//             ESP_LOGI(TAG, "[ * ] Receive music info, sample_rates=%d, bits=%d, ch=%d",
//                      music_info.sample_rates, music_info.bits, music_info.channels);
            
//             // 动态设置 I2S 时钟
//             i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
//             continue;
//         }

//         // 自然播放结束事件：自动切换下一首
//         if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
//             && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
//             && (int)msg.data == AEL_STATUS_STATE_FINISHED) {
            
//             ESP_LOGI(TAG, "[ * ] Track finished. Switching to the next track...");
            
//             audio_pipeline_stop(pipeline);
//             audio_pipeline_wait_for_stop(pipeline);
//             audio_pipeline_reset_ringbuffer(pipeline);
//             audio_pipeline_reset_elements(pipeline);
            
//             set_next_file_marker();
//             audio_pipeline_run(pipeline);
//         }
//     }

//     // 清理资源
//     audio_pipeline_stop(pipeline);
//     audio_pipeline_wait_for_stop(pipeline);
//     audio_pipeline_terminate(pipeline);
//     audio_pipeline_unregister(pipeline, mp3_decoder);
//     audio_pipeline_unregister(pipeline, i2s_stream_writer);
//     audio_pipeline_remove_listener(pipeline);
//     audio_event_iface_destroy(evt);
//     audio_pipeline_deinit(pipeline);
//     audio_element_deinit(i2s_stream_writer);
//     audio_element_deinit(mp3_decoder);
// }

#define WAKENET_ENABLE      (true)
#define MULTINET_ENABLE     (true)

enum _rec_msg_id {
    REC_START = 1,
    REC_STOP,
    REC_CANCEL,
};

static char *TAG = "wwe_example";

// 【已删】删除了 player 句柄
static audio_rec_handle_t     recorder      = NULL;
static audio_element_handle_t raw_read      = NULL;
static QueueHandle_t          rec_q         = NULL;
static bool                   voice_reading = false;

extern esp_websocket_client_handle_t client;

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
                esp_websocket_client_send_bin(client,(const char *)voiceData,ret,portMAX_DELAY);
                printf("[WebSocket] 已发送 %d 字节的音频数据\n", ret);
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
        
        // 【已修】删除了播放“叮咚”音效的代码 esp_audio_sync_play
        
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
        if (voice_reading) {
            int msg = REC_STOP;
            if (xQueueSend(rec_q, &msg, 0) != pdPASS) {
                ESP_LOGE(TAG, "rec stop send failed");
            }
        }

    } else if (AUDIO_REC_WAKEUP_END == event->type) {
        ESP_LOGI(TAG, "rec_engine_cb - REC_EVENT_WAKEUP_END");
        AUDIO_MEM_SHOW(TAG);
    } else if (AUDIO_REC_COMMAND_DECT <= event->type) {
        recorder_sr_mn_result_t *mn_result = event->event_data;

        ESP_LOGI(TAG, "rec_engine_cb - AUDIO_REC_COMMAND_DECT");
        ESP_LOGW(TAG, "command %d, phrase_id %d, prob %f, str: %s"
            , event->type, mn_result->phrase_id, mn_result->prob, mn_result->str);
            
        // 【已修】删除了播放“好的”音效的代码 esp_audio_sync_play
        
    } else {
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
    cfg.vad_off = 2000;
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
    audio_board_init();
    es7210_adc_set_volume(GAIN_37_5DB);
    //setup_player();
    start_recorder();
     rec_q = xQueueCreate(3, sizeof(int));
    audio_thread_create(NULL, "read_task", voice_read_task, NULL, 4 * 1024, 5, true, 0);
}
