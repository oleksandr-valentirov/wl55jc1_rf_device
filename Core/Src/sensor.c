/* The node's own two measurements, off the ADC's internal channels.
 * radio_devices_docs/wl55_device/arch/sensors.md */
#include "sensor.h"

#include "main.h"
#include "timebase.h"

extern ADC_HandleTypeDef hadc;

/* Slower than the tightest grant, so no report ever waits on a conversion.
 * radio_devices_docs/wl55_device/arch/sensors.md */
#define SENSOR_PERIOD_MS   5000u
/* The conversion itself is 14 us; anything near this is a stuck peripheral. */
#define SENSOR_POLL_MS        5u

static sensor_reading_t last;
static uint32_t next_ms, taken_n, failed_n;
static uint8_t  calibrated, armed;

/* One channel, disabled again afterwards: the next one must start from the same state. */
static int convert(uint32_t channel, uint32_t *raw) {
    ADC_ChannelConfTypeDef sel = {0};

    sel.Channel = channel;
    sel.Rank = ADC_REGULAR_RANK_1;
    sel.SamplingTime = ADC_SAMPLINGTIME_COMMON_1;
    if (HAL_ADC_ConfigChannel(&hadc, &sel) != HAL_OK)
        return -1;
    if (HAL_ADC_Start(&hadc) != HAL_OK)
        return -1;
    if (HAL_ADC_PollForConversion(&hadc, SENSOR_POLL_MS) != HAL_OK) {
        (void)HAL_ADC_Stop(&hadc);
        return -1;
    }
    *raw = HAL_ADC_GetValue(&hadc);
    (void)HAL_ADC_Stop(&hadc);
    /* Deselected, or the next channel joins this one rather than replacing it. */
    sel.Rank = ADC_RANK_NONE;
    (void)HAL_ADC_ConfigChannel(&hadc, &sel);
    return 0;
}

/* Tenths, so the 12-bit step - about 0.4 C - is not rounded away.
 * radio_devices_docs/wl55_device/arch/sensors.md */
static int temp_from_raw(uint32_t raw, uint16_t vdda_mv, int16_t *out) {
    int32_t cal1 = (int32_t)*TEMPSENSOR_CAL1_ADDR;
    int32_t cal2 = (int32_t)*TEMPSENSOR_CAL2_ADDR;
    int32_t span = cal2 - cal1;
    int32_t at_cal_vdda, tenths;

    /* An uncalibrated part is not a cold one: no reading beats a wrong one. */
    if (span == 0)
        return -1;
    /* The reading is scaled to the supply the calibration points were taken at. */
    at_cal_vdda = (int32_t)((raw * vdda_mv) / TEMPSENSOR_CAL_VREFANALOG);
    tenths = ((at_cal_vdda - cal1) * (TEMPSENSOR_CAL2_TEMP - TEMPSENSOR_CAL1_TEMP) * 10)
             / span + TEMPSENSOR_CAL1_TEMP * 10;
    out[0] = (int16_t)tenths;
    return 0;
}

void sensor_service(void) {
    uint32_t now = millis_hw(), raw_vref = 0, raw_temp = 0;
    sensor_reading_t r = {0};

    if (armed && (int32_t)(now - next_ms) < 0)
        return;
    armed = 1;
    next_ms = now + SENSOR_PERIOD_MS;

    /* Once: the factors do not move, and it needs the part disabled. */
    if (!calibrated) {
        if (HAL_ADCEx_Calibration_Start(&hadc) != HAL_OK) {
            failed_n++;
            return;
        }
        calibrated = 1;
    }

    if (convert(ADC_CHANNEL_VREFINT, &raw_vref) != 0 || raw_vref == 0u) {
        failed_n++;
        return;
    }
    /* The supply is measured first because the temperature is scaled by it. */
    r.supply_mv = (uint16_t)__LL_ADC_CALC_VREFANALOG_VOLTAGE(raw_vref,
                                                             LL_ADC_RESOLUTION_12B);
    if (convert(ADC_CHANNEL_TEMPSENSOR, &raw_temp) != 0 ||
        temp_from_raw(raw_temp, r.supply_mv, &r.temp_c_x10) != 0) {
        failed_n++;
        return;
    }
    r.taken_ms = now;
    r.valid = 1;
    last = r;
    taken_n++;
}

int sensor_read(sensor_reading_t *out) {
    if (!last.valid)
        return -1;
    *out = last;
    return 0;
}

void sensor_counts(uint32_t *taken, uint32_t *failed) {
    *taken = taken_n;
    *failed = failed_n;
}
