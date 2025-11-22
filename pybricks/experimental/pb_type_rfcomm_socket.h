// SPDX-License-Identifier: MIT
// Copyright (c) 2025 The Pybricks Authors

#ifndef PYBRICKS_INCLUDED_PYBRICKS_EXPERIMENTAL_PB_TYPE_RFCOMM_SOCKET_H
#define PYBRICKS_INCLUDED_PYBRICKS_EXPERIMENTAL_PB_TYPE_RFCOMM_SOCKET_H

#include "py/mpconfig.h"

#if PYBRICKS_PY_COMMON_BTC

#include <pbdrv/bluetooth_classic.h>
#include "py/obj.h"

extern const mp_obj_type_t pb_type_rfcomm_socket;

mp_obj_t pb_type_rfcomm_socket_new(const pbdrv_bluetooth_rfcomm_conn_t *conn);

#endif // PYBRICKS_PY_COMMON_BTC

#endif // PYBRICKS_INCLUDED_PYBRICKS_EXPERIMENTAL_PB_TYPE_RFCOMM_SOCKET_H
