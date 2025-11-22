// SPDX-License-Identifier: MIT
// Copyright (c) 2025 The Pybricks Authors

#include "py/mpconfig.h"

#if PYBRICKS_PY_COMMON_BTC

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <pbdrv/bluetooth_classic.h>
#include <pbio/error.h>

#include "py/obj.h"
#include "py/runtime.h"
#include "py/stream.h"

#include "pb_type_rfcomm_socket.h"
#include "pybricks/tools/pb_type_async.h"

typedef struct {
    mp_obj_base_t base;
    pbdrv_bluetooth_rfcomm_conn_t conn;
} pb_type_rfcomm_socket_obj_t;


static mp_uint_t pb_type_rfcomm_socket_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode) {
    pb_type_rfcomm_socket_obj_t *self = MP_OBJ_TO_PTR(self_in);
    size_t bytes_received;
    pbio_error_t err = pbdrv_bluetooth_rfcomm_recv(&self->conn, buf, size, &bytes_received);
    if (err != PBIO_SUCCESS) {
        *errcode = MP_EIO;
        return MP_STREAM_ERROR;
    }
    return bytes_received;
}

static mp_uint_t pb_type_rfcomm_socket_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode) {
    pb_type_rfcomm_socket_obj_t *self = MP_OBJ_TO_PTR(self_in);
    size_t bytes_sent;
    pbio_error_t err = pbdrv_bluetooth_rfcomm_send(&self->conn, buf, size, &bytes_sent);
    if (err != PBIO_SUCCESS) {
        *errcode = MP_EIO;
        return MP_STREAM_ERROR;
    }
    return bytes_sent;
}

static mp_uint_t pb_type_rfcomm_socket_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    pb_type_rfcomm_socket_obj_t *self = MP_OBJ_TO_PTR(self_in);

    switch (request) {
    case MP_STREAM_POLL: {
        mp_uint_t flags = 0;
        if ((request & MP_STREAM_POLL_HUP) && !pbdrv_bluetooth_rfcomm_is_connected(&self->conn)) {
            flags |= MP_STREAM_POLL_HUP;
        }
        if ((request & MP_STREAM_POLL_RD) && pbdrv_bluetooth_rfcomm_is_readable(&self->conn)) {
            flags |= MP_STREAM_POLL_RD;
        }
        if ((request & MP_STREAM_POLL_WR) && pbdrv_bluetooth_rfcomm_is_writeable(&self->conn)) {
            flags |= MP_STREAM_POLL_WR;
        }
        return flags;
    }
    case MP_STREAM_CLOSE: {
        pbdrv_bluetooth_rfcomm_close(&self->conn);
        return 0;
    }
    case MP_STREAM_FLUSH: {
        // No buffering, so nothing to flush.
        return 0;
    }
    default:
        return MP_STREAM_ERROR;
    }
}

mp_stream_p_t pb_type_rfcomm_socket_stream_p = {
    .read = pb_type_rfcomm_socket_read,
    .write = pb_type_rfcomm_socket_write,
    .ioctl = pb_type_rfcomm_socket_ioctl,
};

static void pb_type_rfcomm_socket_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    pb_type_rfcomm_socket_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "RFCOMMSocket(conn_id=%d)", self->conn.conn_id);
}

static const mp_rom_map_elem_t pb_type_rfcomm_socket_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read),       MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto),  MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_write),       MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_close),      MP_ROM_PTR(&mp_stream_close_obj) },
};
static MP_DEFINE_CONST_DICT(pb_type_rfcomm_socket_locals_dict, pb_type_rfcomm_socket_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(pb_type_rfcomm_socket,
    MP_QSTR_rfcomm_socket,
    MP_TYPE_FLAG_NONE,
    make_new, pb_type_rfcomm_socket_new,
    print, pb_type_rfcomm_socket_print,
    protocol, &pb_type_rfcomm_socket_stream_p,
    locals_dict, &pb_type_rfcomm_socket_locals_dict
);

mp_obj_t pb_type_rfcomm_socket_new(const pbdrv_bluetooth_rfcomm_conn_t *conn) {
    pb_type_rfcomm_socket_obj_t *self = mp_obj_malloc(pb_type_rfcomm_socket_obj_t, &pb_type_rfcomm_socket);
    self->conn = *conn;
    return MP_OBJ_FROM_PTR(self);
}

#endif // PYBRICKS_PY_COMMON_BTC
