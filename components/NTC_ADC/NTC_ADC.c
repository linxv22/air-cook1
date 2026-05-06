#include "NTC_ADC.h"

#include "esp_log.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_continuous.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_events.h"

static const char *TAG = "NTC_ADC";

#define ADC_COUNT 16 // 设置 ADC 读取的样本数量，必须是 4 的倍数，因为每个样本占用 4 字节（包含 unit、channel、raw_data 和 valid 字段）

#define ADC_READ_LEN  4*ADC_COUNT // 以字节为单位设置 ADC 读取缓冲区的大小，必须是 4 的倍数，因为每个样本占用 4 字节（包含 unit、channel、raw_data 和 valid 字段）

static int result [ADC_COUNT]={0}; 

static adc_continuous_handle_t handle = NULL;
static adc_cali_handle_t cali_handle = NULL; 

void ntc_adc_task(void *arg) 
{
  adc_continuous_data_t parsed_data[ADC_COUNT];  // 用户指定最大样本数
  while(1)
  {
   uint32_t num_samples = 0;
   esp_err_t ret = adc_continuous_read_parse(handle, parsed_data, ADC_COUNT , &num_samples, 1000);
   if (ret == ESP_OK) {
      for (int i = 0; i < num_samples ; i++) {
         if (parsed_data[i].valid) {
              ESP_LOGI(TAG, "ADC%d, Channel: %d, Value: %"PRIu32,
                     parsed_data[i].unit + 1,
                     parsed_data[i].channel,
                     parsed_data[i].raw_data);
              adc_cali_raw_to_voltage(cali_handle, parsed_data[i].raw_data,&result[i]);  
              ESP_LOGI(TAG, "Voltage: %d mV", result[i]);
        }
         
    }
}
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}


void ntc_adc_init(void) 
{
    
//创建 ADC 连续转换模式驱动的句柄：  
  adc_continuous_handle_cfg_t adc_config = {
    .conv_frame_size = 1024, // 以字节为单位设置 ADC 转换帧大小
    .max_store_buf_size = 1024, // 以字节为单位设置最大缓冲池的大小，驱动程序将 ADC 转换结果保存到该缓冲池中。
    .flags.flush_pool = 1, // 设置可以改变驱动程序行为的标志。flush_pool：缓冲池满时自动清空缓冲池中的旧数据，重新写入新数据。否则，缓冲池满时，新的数据将丢失。
  };
  ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_config, &handle));

  // adc_iir_filter_handle_t iir_filter_handle = NULL;
  // adc_continuous_iir_filter_config_t iir_filter_config = {
  //    .unit=ADC_UNIT_1, //ADC 单元
  //    .channel = 0, // 设置要过滤的 ADC 通道
  //    .filter_coefficient = 0.1, // 设置 IIR 滤波器的滤波系数，范围为 0.0 到 1.0。较小的值会产生更平滑的输出，但响应较慢；较大的值会产生更快的响应，但可能会有更多的噪声。
  // };
  // ESP_ERROR_CHECK(adc_new_continuous_iir_filter(handle, &iir_filter_config, &iir_filter_handle));
  // ESP_ERROR_CHECK(adc_continuous_iir_filter_enable(handle, iir_filter_handle));
  
  adc_continuous_config_t dig_cfg = {
    .sample_freq_hz = 20 * 1000, // 期望的 ADC 采样频率，单位为 Hz。
    .conv_mode = ADC_CONV_SINGLE_UNIT_1, // 连续转换模式
    .pattern_num = 1, // 要使用的 ADC 通道数量
  };

  adc_digi_pattern_config_t adc_pattern = {
    .atten = ADC_ATTEN_DB_12, //ADC 衰减。请参阅 技术参考手册 中的 ADC 特性 章节
    .channel = ADC_CHANNEL_5, // IO 对应的 ADC 通道号，请参阅下文注意事项。
    .unit = ADC_UNIT_1, // IO 所属的 ADC 单元。
    .bit_width = ADC_BITWIDTH_12, // 原始转换结果的位宽。
  };

  dig_cfg.adc_pattern = &adc_pattern;

  ESP_ERROR_CHECK(adc_continuous_config(handle, &dig_cfg));
  ESP_ERROR_CHECK(adc_continuous_start(handle));

  ESP_LOGI(TAG, "calibration scheme version is %s", "Curve Fitting");
  adc_cali_curve_fitting_config_t cali_config = {
    .unit_id = ADC_UNIT_1,
    .atten = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_DEFAULT,
};
ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle));

  xTaskCreate(ntc_adc_task, "NTC_ADC_Task", 4096, NULL, 5, NULL);
}

float ntc_adc_read_temperature(void) {
    // Read the ADC value and convert it to temperature
    ;
    return 0.0; // Placeholder return value
}
