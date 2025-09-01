// SPDX-License-Identifier: MIT
// Copyright (c) 2019-2022 The Pybricks Authors

#include <pbdrv/config.h>

#if PBDRV_CONFIG_IOPORT

#include <contiki.h>

#include <pbdrv/ioport.h>
#include <pbdrv/uart.h>

pbio_error_t pbdrv_ioport_p5p6_set_mode(const pbdrv_ioport_pins_t *pins, pbdrv_ioport_p5p6_mode_t mode) {

    if (mode == PBDRV_IOPORT_P5P6_MODE_GPIO_ADC) {

        // Reset pins if this port has GPIO pins.
        if (!pins) {
            return PBIO_ERROR_NOT_SUPPORTED;
        }

        pbdrv_gpio_input(&pins->p5);
        pbdrv_gpio_input(&pins->p6);
        pbdrv_gpio_input(&pins->uart_tx);
        pbdrv_gpio_input(&pins->uart_rx);
        pbdrv_gpio_out_high(&pins->uart_buf);

        // These should be set by default already, but it seems that the
        // bootloader on the Technic hub changes these and causes wrong
        // detection if we don't make sure pull is disabled.
        pbdrv_gpio_set_pull(&pins->p5, PBDRV_GPIO_PULL_NONE);
        pbdrv_gpio_set_pull(&pins->p6, PBDRV_GPIO_PULL_NONE);
        pbdrv_gpio_set_pull(&pins->uart_buf, PBDRV_GPIO_PULL_NONE);
        pbdrv_gpio_set_pull(&pins->uart_tx, PBDRV_GPIO_PULL_NONE);
        pbdrv_gpio_set_pull(&pins->uart_rx, PBDRV_GPIO_PULL_NONE);

        return PBIO_SUCCESS;
    } else if (mode == PBDRV_IOPORT_P5P6_MODE_UART) {
        // First reset all pins to inputs by going to GPIO mode recursively.
        pbio_error_t err = pbdrv_ioport_p5p6_set_mode(pins, PBDRV_IOPORT_P5P6_MODE_GPIO_ADC);
        if (err != PBIO_SUCCESS) {
            return err;
        }
        // Sets up alt mode and enables the buffer for UART use.
        pbdrv_gpio_alt(&pins->uart_rx, pins->uart_rx_alt_uart);
        pbdrv_gpio_alt(&pins->uart_tx, pins->uart_tx_alt_uart);
        pbdrv_gpio_out_low(&pins->uart_buf);
        return PBIO_SUCCESS;
    } else if (mode == PBDRV_IOPORT_P5P6_MODE_I2C) {
        // First reset all pins to inputs by going to GPIO mode recursively.
        pbio_error_t err = pbdrv_ioport_p5p6_set_mode(pins, PBDRV_IOPORT_P5P6_MODE_GPIO_ADC);
        if (err != PBIO_SUCCESS) {
            return err;
        }
        pbdrv_gpio_out_low(&pins->p5);
        pbdrv_gpio_input(&pins->p5);
        pbdrv_gpio_out_low(&pins->p6);
        pbdrv_gpio_input(&pins->p6);
        return PBIO_SUCCESS;
    } else if (mode == PBDRV_IOPORT_P5P6_MODE_QUADRATURE) {
        // In PoweredUP, this is only used for two motors in boost. Its counter
        // driver does all the required setup. Its mode can never change. The
        // initial driver init does not check errors for default modes since
        // they are supported by definition. We can return an error for all
        // other ports.
        return PBIO_ERROR_NOT_SUPPORTED;
    }

    return PBIO_ERROR_NOT_SUPPORTED;
}

#if PBDRV_CONFIG_HAS_PORT_VCC_CONTROL
void pbdrv_ioport_enable_vcc(bool enable) {
    if (enable) {
        pbdrv_gpio_out_high(&pbdrv_ioport_platform_data_vcc_pin);
    } else {
        pbdrv_gpio_out_low(&pbdrv_ioport_platform_data_vcc_pin);
    }
}
#endif // PBDRV_CONFIG_HAS_PORT_VCC_CONTROL

#endif // PBDRV_CONFIG_IOPORT
