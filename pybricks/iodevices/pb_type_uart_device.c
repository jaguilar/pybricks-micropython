// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2025 The Pybricks Authors

#include "py/mpconfig.h"

#if PYBRICKS_PY_IODEVICES

#include "py/mphal.h"
#include "py/objstr.h"
#include "py/runtime.h"

#include <contiki.h>

#include <pbdrv/uart.h>
#include <pbio/port_interface.h>

#include <pybricks/common.h>
#include <pybricks/parameters.h>
#include <pybricks/tools/pb_type_async.h>

#include <pybricks/util_mp/pb_kwarg_helper.h>
#include <pybricks/util_mp/pb_obj_helper.h>
#include <pybricks/util_pb/pb_error.h>

// pybricks.iodevices.uart_device class object
typedef struct _pb_type_uart_device_obj_t {
    mp_obj_base_t base;
    pbio_port_t *port;
    pbdrv_uart_dev_t *uart_dev;
    uint32_t timeout;
    pb_type_async_t *write_iter;
    mp_obj_t write_obj;
    pb_type_async_t *read_iter;
    mp_obj_t read_obj;
} pb_type_uart_device_obj_t;

// pybricks.iodevices.UARTDevice.__init__
static mp_obj_t pb_type_uart_device_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    PB_PARSE_ARGS_CLASS(n_args, n_kw, args,
        PB_ARG_REQUIRED(port),
        PB_ARG_DEFAULT_NONE(timeout));

    // Get device, which inits UART port
    pb_type_uart_device_obj_t *self = mp_obj_malloc(pb_type_uart_device_obj_t, type);

    if (timeout_in == mp_const_none) {
        // In the uart driver implementation, 0 means no timeout.
        self->timeout = 0;
    } else {
        // Timeout of 0 is often perceived as partial read if the requested
        // number of bytes is not available. This is not supported, so don't
        // make it appear that way.
        if (pb_obj_get_int(timeout_in) < 1) {
            pb_assert(PBIO_ERROR_INVALID_ARG);
        }
        self->timeout = pb_obj_get_int(timeout_in);
    }

    pbio_port_id_t port_id = pb_type_enum_get_value(port_in, &pb_enum_type_Port);
    pb_assert(pbio_port_get_port(port_id, &self->port));
    pbio_port_set_mode(self->port, PBIO_PORT_MODE_UART);
    pb_assert(pbio_port_get_uart_dev(self->port, &self->uart_dev));
    pbdrv_uart_flush(self->uart_dev);

    // Awaitables associated with reading and writing.
    self->write_iter = NULL;
    self->read_iter = NULL;

    return MP_OBJ_FROM_PTR(self);
}

static pbio_error_t pb_type_uart_device_write_iter_once(pbio_os_state_t *state, mp_obj_t self_in) {
    pb_type_uart_device_obj_t *self = MP_OBJ_TO_PTR(self_in);
    GET_STR_DATA_LEN(self->write_obj, data, data_len);
    return pbdrv_uart_write(state, self->uart_dev, (uint8_t *)data, data_len, self->timeout);
}

static mp_obj_t pb_type_uart_device_write_return_map(mp_obj_t self_in) {
    pb_type_uart_device_obj_t *self = MP_OBJ_TO_PTR(self_in);
    // Write always returns none, but this is effectively a completion callback.
    // So we can use it to disconnect the write object so it can be garbage collected.
    self->write_obj = MP_OBJ_NULL;
    return mp_const_none;
}

// pybricks.iodevices.UARTDevice.write
static mp_obj_t pb_type_uart_device_write(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args,
        pb_type_uart_device_obj_t, self,
        PB_ARG_REQUIRED(data));

    // Assert that data argument are bytes
    if (!(mp_obj_is_str_or_bytes(data_in) || mp_obj_is_type(data_in, &mp_type_bytearray))) {
        pb_assert(PBIO_ERROR_INVALID_ARG);
    }

    // Prevents this object from being garbage collected while the write is in progress.
    self->write_obj = data_in;

    pb_type_async_t config = {
        .iter_once = pb_type_uart_device_write_iter_once,
        .parent_obj = MP_OBJ_FROM_PTR(self),
        .return_map = pb_type_uart_device_write_return_map,
    };
    pb_type_async_schedule_cancel(self->write_iter);
    return pb_type_async_wait_or_await(&config, &self->write_iter);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(pb_type_uart_device_write_obj, 1, pb_type_uart_device_write);

// pybricks.iodevices.UARTDevice.in_waiting
static mp_obj_t pb_type_uart_device_in_waiting(mp_obj_t self_in) {
    pb_type_uart_device_obj_t *self = MP_OBJ_TO_PTR(self_in);
    (void)self;
    pb_assert(PBIO_ERROR_NOT_IMPLEMENTED);
    return mp_obj_new_int(0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(pb_type_uart_device_in_waiting_obj, pb_type_uart_device_in_waiting);

static pbio_error_t pb_type_uart_device_read_iter_once(pbio_os_state_t *state, mp_obj_t self_in) {
    pb_type_uart_device_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_str_t *str = MP_OBJ_TO_PTR(self->read_obj);
    return pbdrv_uart_read(state, self->uart_dev, (uint8_t *)str->data, str->len, self->timeout);
}

static mp_obj_t pb_type_uart_device_read_return_map(mp_obj_t self_in) {
    pb_type_uart_device_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t ret = self->read_obj;
    self->read_obj = MP_OBJ_NULL;
    return ret;
}

// pybricks.iodevices.UARTDevice.read
static mp_obj_t pb_type_uart_device_read(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {

    PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args,
        pb_type_uart_device_obj_t, self,
        PB_ARG_DEFAULT_INT(length, 1));

    // Creates zeroed bytes object of given length, by calling Python function bytes(length).
    mp_obj_t args[] = { length_in };
    self->read_obj = MP_OBJ_TYPE_GET_SLOT(&mp_type_bytes, make_new)((mp_obj_t)&mp_type_bytes, MP_ARRAY_SIZE(args), 0, args);

    pb_type_async_t config = {
        .iter_once = pb_type_uart_device_read_iter_once,
        .parent_obj = MP_OBJ_FROM_PTR(self),
        .return_map = pb_type_uart_device_read_return_map,
    };
    pb_type_async_schedule_cancel(self->read_iter);
    return pb_type_async_wait_or_await(&config, &self->read_iter);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(pb_type_uart_device_read_obj, 1, pb_type_uart_device_read);

// pybricks.iodevices.UARTDevice.set_baudrate
static mp_obj_t pb_type_uart_device_set_baudrate(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    PB_PARSE_ARGS_METHOD(n_args, pos_args, kw_args,
        pb_type_uart_device_obj_t, self,
        PB_ARG_REQUIRED(baudrate));

    pbdrv_uart_set_baud_rate(self->uart_dev, pb_obj_get_int(baudrate_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(pb_type_uart_device_set_baudrate_obj, 1, pb_type_uart_device_set_baudrate);

// pybricks.iodevices.UARTDevice.flush
static mp_obj_t pb_type_uart_device_flush(mp_obj_t self_in) {
    pb_type_uart_device_obj_t *self = MP_OBJ_TO_PTR(self_in);
    pbdrv_uart_flush(self->uart_dev);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(pb_type_uart_device_flush_obj, pb_type_uart_device_flush);

// dir(pybricks.iodevices.uart_device)
static const mp_rom_map_elem_t pb_type_uart_device_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read),         MP_ROM_PTR(&pb_type_uart_device_read_obj)         },
    { MP_ROM_QSTR(MP_QSTR_write),        MP_ROM_PTR(&pb_type_uart_device_write_obj)        },
    { MP_ROM_QSTR(MP_QSTR_in_waiting),   MP_ROM_PTR(&pb_type_uart_device_in_waiting_obj)   },
    { MP_ROM_QSTR(MP_QSTR_set_baudrate), MP_ROM_PTR(&pb_type_uart_device_set_baudrate_obj) },
    { MP_ROM_QSTR(MP_QSTR_flush),        MP_ROM_PTR(&pb_type_uart_device_flush_obj)        },
};
static MP_DEFINE_CONST_DICT(pb_type_uart_device_locals_dict, pb_type_uart_device_locals_dict_table);

// type(pybricks.iodevices.uart_device)
MP_DEFINE_CONST_OBJ_TYPE(pb_type_uart_device,
    MP_QSTR_uart_device,
    MP_TYPE_FLAG_NONE,
    make_new, pb_type_uart_device_make_new,
    locals_dict, &pb_type_uart_device_locals_dict);

#endif // PYBRICKS_PY_IODEVICES
