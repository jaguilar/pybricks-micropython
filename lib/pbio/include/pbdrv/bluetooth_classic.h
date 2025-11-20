#ifndef PBIO_INCLUDE_PBDRV_BLUETOOTH_CLASSIC_H
#define PBIO_INCLUDE_PBDRV_BLUETOOTH_CLASSIC_H

#include <pbdrvconfig.h>

#if PBDRV_CONFIG_BLUETOOTH_CLASSIC

#include <stdint.h>
#include <stdbool.h>

#include <pbio/error.h>
#include <pbio/os.h>

/**
 * Initializes the Bluetooth driver.
 */
void pbdrv_bluetooth_init(void);

/**
 * Deinitializes the Bluetooth driver.
 */
void pbdrv_bluetooth_deinit(void);

/**
 * Gets the bluetooth hub name.
 */
const char *pbdrv_bluetooth_get_hub_name(void);

// A single result from an inquiry scan.
typedef struct {
    uint8_t bdaddr[6];
    int8_t rssi;
    char name[249];
    uint32_t class_of_device;
} pbdrv_bluetooth_inquiry_result_t;

// Callback for handling inquiry results.
typedef void (*pbdrv_bluetooth_inquiry_result_handler_t)(void *context, const pbdrv_bluetooth_inquiry_result_t *result);

/**
 * Runs a bluetooth inquiry scan. Only one such scan can be active at a time.
 *
 * @param [in] state           Protothread state.
 * @param [in] max_responses   Maximum number of responses to report. Use -1 for unlimited.
 * @param [in] timeout         Timeout in units of 1.28 seconds. Values less than one will be coerced to one.
 * @param [in] context         Context pointer to be passed to the result handler.
 * @param [in] result_handler  Callback that will be called for each inquiry result.
 */
pbio_error_t pbdrv_bluetooth_inquiry_scan(
    pbio_os_state_t *state,
    int32_t max_responses,
    int32_t timeout,
    void *context,
    pbdrv_bluetooth_inquiry_result_handler_t result_handler);

#endif  // PBDRV_CONFIG_BLUETOOTH_CLASSIC

#endif
