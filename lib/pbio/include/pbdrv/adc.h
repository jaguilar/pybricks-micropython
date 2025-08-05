// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2025 The Pybricks Authors

/**
 * @addtogroup AnalogDriver Driver: Analog/digital converter (ADC)
 * @{
 */

#ifndef _PBDRV_ADC_H_
#define _PBDRV_ADC_H_

#include <stdint.h>

#include <pbdrv/config.h>
#include <pbio/error.h>
#include <pbio/os.h>

typedef void (*pbdrv_adc_callback_t)(void);

#if PBDRV_CONFIG_ADC

/**
 * Gets the raw analog value for the specified channel.
 * @param [in]  ch      The A/DC channel
 * @param [out] value   The raw value
 * @return              ::PBIO_SUCCESS on success ::PBIO_ERROR_INVALID_ARG if
 *                      the channel is not valid or ::PBIO_ERROR_IO if there
 *                      was an I/O error.
 */
pbio_error_t pbdrv_adc_get_ch(uint8_t ch, uint16_t *value);

/**
 * Awaits for ADC to have new samples ready to be read.
 *
 * Not implemented on all platforms.
 *
 * @param [in]  state          Protothread state.
 * @param [out] start_time_us  Persistent value used by this function to store the start time (µs).
 * @param [in]  future_us      How far into the future the sample should be (µs).
 */
pbio_error_t pbdrv_adc_await_new_samples(pbio_os_state_t *state, uint32_t *start_time_us, uint32_t future_us);

#else

static inline pbio_error_t pbdrv_adc_get_ch(uint8_t ch, uint16_t *value) {
    *value = 0;
    return PBIO_ERROR_NOT_SUPPORTED;
}

static inline pbio_error_t pbdrv_adc_await_new_samples(pbio_os_state_t *state, uint32_t *start_time_us, uint32_t future_us) {
    return PBIO_ERROR_NOT_SUPPORTED;
}

#endif

#endif /* _PBDRV_ADC_H_ */

/** @} */
