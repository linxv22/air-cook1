#include "my_audio.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_mem.h"
#include "audio_common.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "board.h"

static const char *TAG = "my_audio";

static struct marker {
    int pos;
    const uint8_t *start;
    const uint8_t *end;
} file_marker;

// 嵌入的三个不同采样率的 MP3 文件
extern const uint8_t lr_mp3_start[] asm("_binary_music_16b_2c_8000hz_mp3_start");
extern const uint8_t lr_mp3_end[]   asm("_binary_music_16b_2c_8000hz_mp3_end");

extern const uint8_t mr_mp3_start[] asm("_binary_music_16b_2c_22050hz_mp3_start");
extern const uint8_t mr_mp3_end[]   asm("_binary_music_16b_2c_22050hz_mp3_end");

extern const uint8_t hr_mp3_start[] asm("_binary_music_16b_2c_44100hz_mp3_start");
extern const uint8_t hr_mp3_end[]   asm("_binary_music_16b_2c_44100hz_mp3_end");

static void set_next_file_marker(void)
{
    static int idx = 0;

    switch (idx) {
        case 0:
            file_marker.start = lr_mp3_start;
            file_marker.end   = lr_mp3_end;
            ESP_LOGI(TAG, "[ * ] Switch to: 8000Hz MP3");
            break;
        case 1:
            file_marker.start = mr_mp3_start;
            file_marker.end   = mr_mp3_end;
            ESP_LOGI(TAG, "[ * ] Switch to: 22050Hz MP3");
            break;
        case 2:
            file_marker.start = hr_mp3_start;
            file_marker.end   = hr_mp3_end;
            ESP_LOGI(TAG, "[ * ] Switch to: 44100Hz MP3");
            break;
        default:
            ESP_LOGE(TAG, "[ * ] Unsupported index = %d", idx);
    }
    if (++idx > 2) {
        idx = 0;
    }
    file_marker.pos = 0;
}

int mp3_music_read_cb(audio_element_handle_t el, char *buf, int len, TickType_t wait_time, void *ctx)
{
    int read_size = file_marker.end - file_marker.start - file_marker.pos;
    if (read_size == 0) {
        return AEL_IO_DONE;
    } else if (len < read_size) {
        read_size = len;
    }
    memcpy(buf, file_marker.start + file_marker.pos, read_size);
    file_marker.pos += read_size;
    return read_size;
}

void my_audio_init(void)
{
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t i2s_stream_writer, mp3_decoder;

    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    // ---------------- [ 1 ] 初始化音频板及 Codec 芯片 ----------------
    ESP_LOGI(TAG, "[ 1 ] Start audio codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    int player_volume = 100; 
    audio_hal_set_volume(board_handle->audio_hal, player_volume);

    // ---------------- [ 2 ] 创建并初始化管道 ----------------
    ESP_LOGI(TAG, "[ 2 ] Create audio pipeline");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    // ---------------- [ 2.1 ] 创建 MP3 解码器 ----------------
    ESP_LOGI(TAG, "[2.1] Create mp3 decoder");
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    mp3_decoder = mp3_decoder_init(&mp3_cfg);
    audio_element_set_read_cb(mp3_decoder, mp3_music_read_cb, NULL);

    // ---------------- [ 2.2 ] 创建 I2S 输出流 ----------------
    ESP_LOGI(TAG, "[2.2] Create i2s stream");
    
#if defined CONFIG_ESP32_C3_LYRA_V2_BOARD
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_PDM_TX_CFG_DEFAULT();
#else
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
#endif

   

    i2s_stream_writer = i2s_stream_init(&i2s_cfg);

    // ---------------- [ 2.3 ] 注册与链接 ----------------
    ESP_LOGI(TAG, "[2.3] Register elements to pipeline");
    audio_pipeline_register(pipeline, mp3_decoder, "mp3");
    audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

    ESP_LOGI(TAG, "[2.4] Link elements");
    const char *link_tag[2] = {"mp3", "i2s"};
    audio_pipeline_link(pipeline, &link_tag[0], 2);

    // ---------------- [ 4 ] 配置事件监听器 ----------------
    ESP_LOGI(TAG, "[ 4 ] Set up event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[4.1] Listening event from pipeline");
    audio_pipeline_set_listener(pipeline, evt);

    // ---------------- [ 5 ] 启动音频播放 ----------------
    ESP_LOGI(TAG, "[ 5 ] Start audio_pipeline");
    set_next_file_marker(); 
    audio_pipeline_run(pipeline);

    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            continue;
        }

        // 格式上报事件：根据解码出的格式更新 I2S 时钟
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) mp3_decoder
            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(mp3_decoder, &music_info);
            
            ESP_LOGI(TAG, "[ * ] Receive music info, sample_rates=%d, bits=%d, ch=%d",
                     music_info.sample_rates, music_info.bits, music_info.channels);
            
            // 动态设置 I2S 时钟
            i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
            continue;
        }

        // 自然播放结束事件：自动切换下一首
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
            && (int)msg.data == AEL_STATUS_STATE_FINISHED) {
            
            ESP_LOGI(TAG, "[ * ] Track finished. Switching to the next track...");
            
            audio_pipeline_stop(pipeline);
            audio_pipeline_wait_for_stop(pipeline);
            audio_pipeline_reset_ringbuffer(pipeline);
            audio_pipeline_reset_elements(pipeline);
            
            set_next_file_marker();
            audio_pipeline_run(pipeline);
        }
    }

    // 清理资源
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_terminate(pipeline);
    audio_pipeline_unregister(pipeline, mp3_decoder);
    audio_pipeline_unregister(pipeline, i2s_stream_writer);
    audio_pipeline_remove_listener(pipeline);
    audio_event_iface_destroy(evt);
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(i2s_stream_writer);
    audio_element_deinit(mp3_decoder);
}
