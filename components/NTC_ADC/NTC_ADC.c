#include "NTC_ADC.h"

#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "NTC_ADC";


void ntc_adc_init(void) {
    // Initialize the ADC for NTC thermistor reading
  ;
}


float ntc_adc_read_temperature(void) {
    // Read the ADC value and convert it to temperature
    ;
    return 0.0; // Placeholder return value
}
