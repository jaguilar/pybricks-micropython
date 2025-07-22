// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2024 The Pybricks Authors

// Provides Human Machine Interface (HMI) between hub and user.

// TODO: implement additional buttons and menu system (via matrix display) for SPIKE Prime
// TODO: implement additional buttons and menu system (via screen) for NXT

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <contiki.h>

#include <pbdrv/core.h>
#include <pbdrv/reset.h>
#include <pbdrv/led.h>
#include <pbio/button.h>
#include <pbio/color.h>
#include <pbio/light.h>
#include <pbsys/config.h>
#include <pbsys/main.h>
#include <pbsys/status.h>
#include <pbsys/storage_settings.h>

#include "light_matrix.h"
#include "light.h"

static struct pt update_program_run_button_wait_state_pt;

// The selected slot is not persistent across reboot, so that the first slot
// is always active on boot. This allows consistently starting programs without
// visibility of the display.
static uint8_t selected_slot = 0;

/**
 * Protothread to monitor the button state to trigger starting the user program.
 * @param [in]  button_pressed      The current button state.
 */
static PT_THREAD(update_program_run_button_wait_state(bool button_pressed)) {
    struct pt *pt = &update_program_run_button_wait_state_pt;

    // This should not be active while a program is running.
    if (pbsys_status_test(PBIO_PYBRICKS_STATUS_USER_PROGRAM_RUNNING)) {
        PT_EXIT(pt);
    }

    PT_BEGIN(pt);

    for (;;) {
        // button may still be pressed from power on or user program stop
        PT_WAIT_UNTIL(pt, !button_pressed);
        PT_WAIT_UNTIL(pt, button_pressed);
        PT_WAIT_UNTIL(pt, !button_pressed);

        // If we made it through a full press and release, without the user
        // program running, then start the currently selected user program.
        pbsys_main_program_request_start(selected_slot, PBSYS_MAIN_PROGRAM_START_REQUEST_TYPE_HUB_UI);
    }

    PT_END(pt);
}

#if PBSYS_CONFIG_BLUETOOTH_TOGGLE

static struct pt update_bluetooth_button_wait_state_pt;

/**
 * Protothread to monitor the button state to toggle Bluetooth.
 * @param [in]  button_pressed      The current button state.
 */
static PT_THREAD(update_bluetooth_button_wait_state(bool button_pressed)) {
    struct pt *pt = &update_bluetooth_button_wait_state_pt;

    // This should not be active while a program is running.
    if (pbsys_status_test(PBIO_PYBRICKS_STATUS_USER_PROGRAM_RUNNING)) {
        PT_EXIT(pt);
    }

    PT_BEGIN(pt);

    for (;;) {
        // button may still be pressed during user program
        PT_WAIT_UNTIL(pt, !button_pressed);
        PT_WAIT_UNTIL(pt, button_pressed);
        pbsys_storage_settings_bluetooth_enabled_request_toggle();
    }

    PT_END(pt);
}

#endif // PBSYS_CONFIG_BLUETOOTH_TOGGLE

#if PBSYS_CONFIG_HMI_NUM_SLOTS

static struct pt update_left_right_button_wait_state_pt;

/**
 * Gets the currently selected program slot.
 *
 * @return The currently selected program slot (zero-indexed).
 */
uint8_t pbsys_hmi_get_selected_program_slot(void) {
    return selected_slot;
}

/**
 * Protothread to monitor the left and right button state to select a slot.
 *
 * @param [in]  left_button_pressed      The current left button state.
 * @param [in]  right_button_pressed     The current right button state.
 */
static PT_THREAD(update_left_right_button_wait_state(bool left_button_pressed, bool right_button_pressed)) {
    struct pt *pt = &update_left_right_button_wait_state_pt;

    // This should not be active while a program is running.
    if (pbsys_status_test(PBIO_PYBRICKS_STATUS_USER_PROGRAM_RUNNING)) {
        PT_EXIT(pt);
    }

    static uint8_t previous_slot;
    static uint32_t first_press_time;

    PT_BEGIN(pt);

    for (;;) {
        // Buttons may still be pressed during user program
        PT_WAIT_UNTIL(pt, !left_button_pressed && !right_button_pressed);

        // Wait for either button.
        PT_WAIT_UNTIL(pt, left_button_pressed || right_button_pressed);

        first_press_time = pbdrv_clock_get_ms();

        // On right, increment slot when possible.
        if (right_button_pressed && selected_slot < 4) {
            selected_slot++;
            pbsys_hub_light_matrix_update_program_slot();
        }
        // On left, decrement slot when possible.
        if (left_button_pressed && selected_slot > 0) {
            selected_slot--;
            pbsys_hub_light_matrix_update_program_slot();
        }

        // Next state could be either both pressed or both released.
        PT_WAIT_UNTIL(pt, left_button_pressed == right_button_pressed);

        // If both were pressed soon after another, user wanted to start port view,
        // not switch programs, so revert slot change.
        if (left_button_pressed && pbdrv_clock_get_ms() - first_press_time < 100) {
            selected_slot = previous_slot;
            pbsys_hub_light_matrix_update_program_slot();
            pbsys_main_program_request_start(PBIO_PYBRICKS_USER_PROGRAM_ID_PORT_VIEW, PBSYS_MAIN_PROGRAM_START_REQUEST_TYPE_HUB_UI);
        } else {
            // Successful switch. And UI was already updated.
            previous_slot = selected_slot;
        }
    }

    PT_END(pt);
}

#endif // PBSYS_CONFIG_HMI_NUM_SLOTS

void pbsys_hmi_init(void) {
    pbsys_status_light_init();
    pbsys_hub_light_matrix_init();
    PT_INIT(&update_program_run_button_wait_state_pt);

    #if PBSYS_CONFIG_BLUETOOTH_TOGGLE
    PT_INIT(&update_bluetooth_button_wait_state_pt);
    #endif // PBSYS_CONFIG_BLUETOOTH_TOGGLE
}

void pbsys_hmi_handle_status_change(pbsys_status_change_t event, pbio_pybricks_status_t data) {
    pbsys_status_light_handle_status_change(event, data);
}

/**
 * Polls the HMI.
 *
 * This is called periodically to update the current HMI state.
 */
void pbsys_hmi_poll(void) {
    pbio_button_flags_t btn = pbdrv_button_get_pressed();

    if (btn & PBIO_BUTTON_CENTER) {
        pbsys_status_set(PBIO_PYBRICKS_STATUS_POWER_BUTTON_PRESSED);
        update_program_run_button_wait_state(true);

        // power off when button is held down for 2 seconds
        if (pbsys_status_test_debounce(PBIO_PYBRICKS_STATUS_POWER_BUTTON_PRESSED, true, 2000)) {
            pbsys_status_set(PBIO_PYBRICKS_STATUS_SHUTDOWN_REQUEST);
        }
    } else {
        pbsys_status_clear(PBIO_PYBRICKS_STATUS_POWER_BUTTON_PRESSED);
        update_program_run_button_wait_state(false);
    }

    #if PBSYS_CONFIG_BLUETOOTH_TOGGLE
    update_bluetooth_button_wait_state(btn & PBSYS_CONFIG_BLUETOOTH_TOGGLE_BUTTON);
    #endif // PBSYS_CONFIG_BLUETOOTH_TOGGLE

    #if PBSYS_CONFIG_HMI_NUM_SLOTS
    update_left_right_button_wait_state(btn & PBIO_BUTTON_LEFT, btn & PBIO_BUTTON_RIGHT);
    #endif // PBSYS_CONFIG_HMI_NUM_SLOTS

    pbsys_status_light_poll();
}
