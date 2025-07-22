// SPDX-License-Identifier: MIT
// Copyright (c) 2024 The Pybricks Authors

/**
 * @addtogroup SysStorageSettings System: Load user settings.
 *
 * Interface for reading and storing user system settings.
 *
 * @{
 */

#ifndef _PBSYS_STORAGE_SETTINGS_H_
#define _PBSYS_STORAGE_SETTINGS_H_

#include <stdbool.h>
#include <stdint.h>

#include <pbio/error.h>

#include <pbio/config.h>
#include <pbio/imu.h>
#include <pbsys/config.h>

/**
 * System setting flags
 */
typedef enum {
    /**
     * Bluetooth is enabled by the user (defaults to true).
     */
    PBSYS_STORAGE_SETTINGS_FLAGS_BLUETOOTH_ENABLED = (1 << 0),
} pbsys_storage_settings_flags_t;

/**
 * System settings. All data types are little-endian.
 */
typedef struct _pbsys_storage_settings_t {
    /** System setting flags. */
    uint32_t flags;
    #if PBIO_CONFIG_IMU
    pbio_imu_persistent_settings_t imu_settings;
    #endif
} pbsys_storage_settings_t;

#if PBSYS_CONFIG_STORAGE

void pbsys_storage_settings_set_defaults(pbsys_storage_settings_t *settings);

void pbsys_storage_settings_apply_loaded_settings(pbsys_storage_settings_t *settings);

bool pbsys_storage_settings_bluetooth_enabled_get(void);

void pbsys_storage_settings_bluetooth_enabled_set(bool enable);

#else

static inline void pbsys_storage_settings_set_defaults(pbsys_storage_settings_t *settings) {
}
static inline void pbsys_storage_settings_apply_loaded_settings(pbsys_storage_settings_t *settings) {
}
static inline bool pbsys_storage_settings_bluetooth_enabled_get(void) {
    return true;
}
static inline void pbsys_storage_settings_bluetooth_enabled_set(bool enable) {
}

#endif // PBSYS_CONFIG_STORAGE

#endif // _PBSYS_STORAGE_SETTINGS_H_

/** @} */
