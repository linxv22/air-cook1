#include "NTC_ADC.h"

#include "esp_log.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_continuous.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

#include "app_events.h"

static const char *TAG = "NTC_ADC";

#define ADC_COUNT 16 // 设置 ADC 读取的样本数量，必须是 4 的倍数，因为每个样本占用 4 字节（包含 unit、channel、raw_data 和 valid 字段）
#define ADC_READ_LEN  4*ADC_COUNT // 以字节为单位设置 ADC 读取缓冲区的大小，必须是 4 的倍数，因为每个样本占用 4 字节（包含 unit、channel、raw_data 和 valid 字段）

/* NTC 参数 (请根据你的实际硬件原理图修改这些值) */
#define NTC_R25 100000.0f      // NTC在25度时的典型阻值，默认写空气炸锅常用的100K
#define NTC_BETA 3950.0f       // NTC的B值
#define PULLDOWN_R 2000.0f // 串联的上拉（或下拉）分压电阻阻值，2k
#define V_REF 3300.0f          // 3.3V供电即3300mV

static int result = 0; 
static adc_continuous_handle_t handle = NULL;
static adc_cali_handle_t cali_handle = NULL; 
static volatile float current_temperature = 0.0; // 缓存出来的当前最平稳的温度值

/* 根据测得的毫伏电压计算当前温度 */
static float convert_voltage_to_temp(int voltage_mv) {
    if (voltage_mv <= 0) return -273.15f;           // 避免除零或 ADC 异常底层返回负值
    if (voltage_mv >= V_REF - 10) return 999.0f;    // NTC 短路时，电压会接近 3.3V

    // 电路拓扑: 3.3V -> NTC -> [ADC测量点] -> 2K下拉电阻 -> GND
    // 公式: V_out = V_Ref * (PULLDOWN_R) / (R_ntc + PULLDOWN_R)
    float r_ntc = ((V_REF - voltage_mv) * PULLDOWN_R) / voltage_mv;
    
    // 使用标准的Steinhart-Hart方程的B参数简化版: 1/T = 1/T0 + (1/B)*ln(R/R0)
    float temp_k = 1.0f / (1.0f / 298.15f + (1.0f / NTC_BETA) * log(r_ntc / NTC_R25));
    
    return temp_k - 273.15f;  // 转为摄氏度
}

void ntc_adc_task(void *arg) 
{
  adc_continuous_data_t parsed_data[ADC_COUNT];  // 用户指定最大样本数

  while(1)
  {
   uint32_t num_samples = 0;
   uint32_t sum_mv = 0;
   uint32_t valid_count = 0;
   esp_err_t ret = adc_continuous_read_parse(handle, parsed_data, ADC_COUNT , &num_samples, 1000);
   if (ret == ESP_OK) {
      
      for (int i = 0; i < num_samples ; i++) {
         if (parsed_data[i].valid) { 
              adc_cali_raw_to_voltage(cali_handle, parsed_data[i].raw_data, &result);  
              sum_mv += result;
              valid_count++;
        }
      }
      
      // 取平均值，滤除大功率电器造成的毛刺
      if (valid_count > 0) {
          int avg_mv = sum_mv / valid_count;
          current_temperature = convert_voltage_to_temp(avg_mv);
          // 调试时可以打开打印，生产环境建议关闭减小CPU占用
          // ESP_LOGI(TAG, "Avg Vol: %d mV, Temp: %.1f C", avg_mv, current_temperature);
      }
    }

    esp_event_post_to(loop_handle, AIR_COOKER_EVENTS, EVENT_TEMP_UPDATED, NULL, 0, 0); // 发送事件通知主任务风扇转速更新了
    // 1000ms读取一次，符合空气炸锅等大热惯性设备的高效检测周期
    vTaskDelay(pdMS_TO_TICKS(1000));
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

  xTaskCreate(ntc_adc_task, "NTC_ADC_Task", 2048, NULL, 8, NULL);
}

float ntc_adc_read_temperature(void) {
    // 返回最近一次ADC任务在后台计算出的稳定温度值

    // return current_temperature;
    return 100.0f; // 这里先返回一个固定值，后续会改为 current_temperature
}
