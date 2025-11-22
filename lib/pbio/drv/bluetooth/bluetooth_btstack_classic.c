// SPDX-License-Identifier: MIT
// Copyright (c) 2020-2023 The Pybricks Authors

// Bluetooth driver using BlueKitchen BTStack for Classic Bluetooth.

#include <pbdrv/config.h>

#if PBDRV_CONFIG_BLUETOOTH_BTSTACK_CLASSIC

#include "bluetooth_btstack_classic.h"

#include <math.h>
#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include <bluetooth_sdp.h>
#include <btstack.h>
#include <btstack_event.h>
#include <btstack_chipset_cc256x.h>
#include <classic/rfcomm.h>
#include <classic/sdp_client.h>
#include <hci_dump.h>
#include <hci_transport_h4.h>
#include <umm_malloc.h>

#include <pbdrv/bluetooth.h>
#include <pbdrv/clock.h>

#include <pbio/error.h>
#include <pbio/os.h>

#include "bluetooth_init_cc2560x.h"
#include "../uart/uart_debug_first_port.h"
#include <pbsys/storage.h>
#include <pbsys/storage_settings.h>
#include <lwrb/lwrb.h>

#define MAX_NUM_CONNECTIONS (7)
#define RFCOMM_RX_BUFFER_SIZE (4 * 1024)
#define RFCOMM_TX_BUFFER_SIZE (2 * 1024)

#define DEBUG 1
#if DEBUG
// These functions can be useful for debugging, but they aren't usually enabled.
static void pbdrv_hci_dump_reset(void) {
}

static void pbdrv_hci_dump_log_packet(uint8_t packet_type, uint8_t in, uint8_t *packet, uint16_t len) {
    pbdrv_uart_debug_printf("HCI %s packet type: %02x, len: %u\n", in ? "in" : "out", packet_type, len);
}

static void pbdrv_hci_dump_log_message(int log_level, const char *format, va_list argptr) {
    pbdrv_uart_debug_vprintf(format, argptr);
    pbdrv_uart_debug_printf("\n");
}

static const hci_dump_t bluetooth_btstack_classic_hci_dump = {
    .reset = pbdrv_hci_dump_reset,
    .log_packet = pbdrv_hci_dump_log_packet,
    .log_message = pbdrv_hci_dump_log_message,
};

static void bluetooth_btstack_classic_hci_dump_enable(bool enable) {
    hci_dump_enable_packet_log(enable);
    hci_dump_enable_log_level(HCI_DUMP_LOG_LEVEL_INFO, enable);
    hci_dump_enable_log_level(HCI_DUMP_LOG_LEVEL_ERROR, enable);
    hci_dump_enable_log_level(HCI_DUMP_LOG_LEVEL_DEBUG, enable);
}

#define DEBUG_PRINT(...) pbdrv_uart_debug_printf(__VA_ARGS__)
#else
#define DEBUG_PRINT(...)
#define bluetooth_btstack_classic_hci_dump_enable(...)
#endif

static int link_db_entry_size() {
    return sizeof(bd_addr_t) + sizeof(link_key_t) + sizeof(link_key_type_t);
}

static int link_db_overhead() {
    return 2;  // 1 byte for length, 1 byte for checksum.
}

static int link_db_max_entries() {
    const int result = (PBSYS_CONFIG_BLUETOOTH_CLASSIC_LINK_KEY_DB_SIZE - link_db_overhead()) / link_db_entry_size();
    if (result > 255) {
        return 255;  // Only 1 byte to store the count.
    }
    return result;
}

// Computes the checksum for the link db, which we use to verify data integrity.
static uint8_t link_db_compute_checksum(uint8_t *data) {
    uint16_t count = data[0];
    const int end_offset = link_db_overhead() + count * link_db_entry_size();
    uint8_t checksum = 0;
    for (int off = 0; off < end_offset; off++) {
        checksum ^= data[off];
    }
    return checksum;
}

static uint8_t link_db_read_checksum(uint8_t *data) {
    uint16_t count = data[0];
    const int end_offset = link_db_overhead() + count * link_db_entry_size();
    return data[end_offset];
}

// Stores the link db to the settings and requests that it be written to flash.
static void link_db_settings_save(void) {
    btstack_link_key_iterator_t it;
    if (!gap_link_key_iterator_init(&it)) {
        DEBUG_PRINT("Failed to initialize link key iterator\n");
        return;
    }
    uint8_t *base = pbsys_storage_settings_get_link_key_db();
    uint16_t off = 0;

    uint8_t count = 0;
    off += 1;  // The first entry starts at offset 1.

    bdaddr_t addr;
    link_key_t key;
    link_key_type_t type;
    while (count < link_db_max_entries() &&
           gap_link_key_iterator_get_next(&it, addr, key, &type)) {
        memcpy(base + off, addr, sizeof(bd_addr_t));
        off += sizeof(bd_addr_t);
        memcpy(base + off, key, sizeof(link_key_t));
        off += sizeof(link_key_t);
        base[off] = type;
        off += sizeof(link_key_type_t);
        count++;
    }
    gap_link_key_iterator_done(&it);
    base[0] = count;
    base[off] = link_db_compute_checksum(base);
    pbsys_storage_request_write();
    DEBUG_PRINT("Saved %u link keys to settings\n", count);
}

// Loads the link DB from the settings.
static void link_db_settings_load(void) {
    uint8_t *base = pbsys_storage_settings_get_link_key_db();
    uint16_t count = base[0];
    if (count > link_db_max_entries()) {
        DEBUG_PRINT("Link key database has invalid entry count, ignoring: %u\n",
            count);
        return;
    }

    if (link_db_read_checksum(base) != link_db_compute_checksum(base)) {
        DEBUG_PRINT("Link key database has invalid checksum, ignoring.\n");
        return;
    }

    uint16_t off = 1;
    for (uint16_t i = 0; i < count; i++) {
        bdaddr_t addr;
        link_key_t key;
        link_key_type_t type;
        memcpy(addr, base + off, sizeof(bd_addr_t));
        off += sizeof(bd_addr_t);
        memcpy(key, base + off, sizeof(link_key_t));
        off += sizeof(link_key_t);
        type = base[off];
        off += sizeof(link_key_type_t);
        gap_store_link_key_for_bd_addr(addr, key, type);
    }
    DEBUG_PRINT("Loaded %u link keys from settings\n", count);
}

// Returns true if the given address is already paired in our link database.
bool is_already_paired(bd_addr_t addr) {
    link_key_t key;
    link_key_type_t type;
    return gap_get_link_key_for_bd_addr(addr, key, &type);
}

static const pbdrv_bluetooth_btstack_platform_data_t *pdata = &pbdrv_bluetooth_btstack_platform_data;

static const hci_transport_config_uart_t hci_transport_config = {
    .type = HCI_TRANSPORT_CONFIG_UART,
    .baudrate_init = 115200,
    // Note: theoretically the AM1808 should be able to go up to 1875000 or
    // higher, but we observed random lost transfers at that speed. 921600 seems
    // stable and is still plenty of bandwidth for Bluetooth classic.
    .baudrate_main = 921600,
    .flowcontrol = 1,
    .device_name = NULL,
};

typedef struct {
    uint8_t *tx_buffer_data;  // tx_buffer from customer. We don't own this.
    uint8_t *rx_buffer_data;
    pbio_os_timer_t tx_timer;  // Timer for tracking timeouts on the current send.
    pbio_os_timer_t rx_timer;  // Timer for tracking timeouts on the current receive.
    lwrb_t tx_buffer;  // Ring buffer to contain outgoing data.
    lwrb_t rx_buffer;  // Ring buffer to contain incoming data.

    int mtu;           // MTU for this connection.

    // How many rfcomm credits are outstanding? When the connection is first started,
    // this is the rx buffer size divided by the MTU (the frame size). Each time we receive
    // a frame, this decreases by one. When frames are consumed by a reader, or if
    // the discrepancy between what we can hold and what is outstanding grows
    // too large, we grant more credits.
    int credits_outstanding;

    pbio_error_t err;  // The first encountered error.
    uint16_t cid;             // The local rfcomm connection handle.
    uint16_t server_channel;  // The remote rfcomm channel we're connected to.
    bool used;         // Is this socket descriptor in use?
    bool connected;    // Is this socket descriptor connected?
} pbdrv_bluetooth_classic_rfcomm_socket_t;

static pbdrv_bluetooth_classic_rfcomm_socket_t pbdrv_bluetooth_classic_rfcomm_sockets[MAX_NUM_CONNECTIONS];

static int pbdrv_bluetooth_classic_rfcomm_socket_max_credits(pbdrv_bluetooth_classic_rfcomm_socket_t *socket) {
    return RFCOMM_RX_BUFFER_SIZE / socket->mtu;
}

// Give back credits that we owe and which we have space available to serve.
// We will give back credits when:
// 1. We owe two or more.
// 2. We owe one and the peer has no credits available.
// In no event will we give back credits such that the peer has enough credits
// to overflow our rx buffer.
static void pbdrv_bluetooth_classic_rfcomm_socket_grant_owed_credits(pbdrv_bluetooth_classic_rfcomm_socket_t *socket) {
    const int avail_frames = lwrb_get_free(&socket->rx_buffer) / socket->mtu;
    const int max_credits = pbdrv_bluetooth_classic_rfcomm_socket_max_credits(socket);
    int max_grant = max_credits - socket->credits_outstanding;
    if (max_grant > avail_frames) {
        max_grant = avail_frames;
    }
    if (max_grant > 1 || (max_grant > 0 && socket->credits_outstanding <= 1)) {
        rfcomm_grant_credits(socket->cid, max_grant);
        socket->credits_outstanding += max_grant;
    }
}

static void pbdrv_bluetooth_classic_rfcomm_socket_reset(pbdrv_bluetooth_classic_rfcomm_socket_t *socket) {
    if (socket->rx_buffer_data) {
        umm_free(socket->rx_buffer_data);
        socket->rx_buffer_data = NULL;
    }
    if (socket->tx_buffer_data) {
        umm_free(socket->tx_buffer_data);
        socket->tx_buffer_data = NULL;
    }
    lwrb_free(&socket->rx_buffer);
    lwrb_free(&socket->tx_buffer);
    socket->used = false;
    socket->connected = false;
    socket->cid = (uint16_t)-1;
    socket->mtu = 0;
    socket->credits_outstanding = 0;
    socket->err = PBIO_SUCCESS;
}

static pbdrv_bluetooth_classic_rfcomm_socket_t *pbdrv_bluetooth_classic_rfcomm_socket_alloc() {
    for (uint32_t i = 0; i < PBIO_ARRAY_SIZE(pbdrv_bluetooth_classic_rfcomm_sockets); i++) {
        if (!pbdrv_bluetooth_classic_rfcomm_sockets[i].used) {
            pbdrv_bluetooth_classic_rfcomm_socket_t *sock = &pbdrv_bluetooth_classic_rfcomm_sockets[i];
            pbdrv_bluetooth_classic_rfcomm_socket_reset(sock);
            sock->used = true;
            sock->rx_buffer_data = umm_malloc(RFCOMM_RX_BUFFER_SIZE);
            sock->tx_buffer_data = umm_malloc(RFCOMM_TX_BUFFER_SIZE);
            if (!sock->rx_buffer_data) {
                DEBUG_PRINT("Failed to allocate RFCOMM RX buffer.\n");
                sock->used = false;
                return NULL;
            }
            lwrb_init(&sock->rx_buffer, sock->rx_buffer_data, RFCOMM_RX_BUFFER_SIZE);
            lwrb_init(&sock->tx_buffer, sock->tx_buffer_data, RFCOMM_TX_BUFFER_SIZE);
            return sock;
        }
    }
    DEBUG_PRINT("[btc] Alloc failed; all sockets in use.\n");
    return NULL;
}


static pbdrv_bluetooth_classic_rfcomm_socket_t *pbdrv_bluetooth_classic_rfcomm_socket_find_by_cid(uint16_t cid) {
    for (size_t i = 0; i < MAX_NUM_CONNECTIONS; i++) {
        if (pbdrv_bluetooth_classic_rfcomm_sockets[i].used && pbdrv_bluetooth_classic_rfcomm_sockets[i].cid == cid) {
            return &pbdrv_bluetooth_classic_rfcomm_sockets[i];
        }
    }
    return NULL;
}

static pbdrv_bluetooth_classic_rfcomm_socket_t *pbdrv_bluetooth_classic_rfcomm_socket_find_by_conn(const pbdrv_bluetooth_rfcomm_conn_t *c) {
    if (c->conn_id < 0 || c->conn_id >= MAX_NUM_CONNECTIONS) {
        return NULL;
    }
    return &pbdrv_bluetooth_classic_rfcomm_sockets[c->conn_id];
}

static int pbdrv_bluetooth_classic_rfcomm_socket_id(pbdrv_bluetooth_classic_rfcomm_socket_t *socket) {
    for (size_t i = 0; i < MAX_NUM_CONNECTIONS; i++) {
        if (&pbdrv_bluetooth_classic_rfcomm_sockets[i] == socket) {
            return i;
        }
    }
    return -1;
}

typedef struct {
    uint8_t status;
    uint8_t hci_version;
    uint16_t hci_revision;
    uint8_t lmp_pal_version;
    uint16_t manufacturer;
    uint16_t lmp_pal_subversion;
} pbdrv_bluetooth_local_version_info_t;

// Pending request datastructures. There is a common pattern to how we handle
// all of the operations in this file.
//
// * The caller *sets* the pending request variable or handler.
// * The packet handler *fills* the pending request variable or calls the handler.
// * The packet handler *clears* the pending request variable or handler to indicate operation completion.
static pbdrv_bluetooth_local_version_info_t *pending_local_version_info_request;
static int32_t pending_inquiry_response_count;
static int32_t pending_inquiry_response_limit;
static void *pending_inquiry_result_handler_context;
static pbdrv_bluetooth_inquiry_result_handler_t pending_inquiry_result_handler;
static pbdrv_bluetooth_classic_rfcomm_socket_t *pending_listen_socket;


static void pbdrv_bluetooth_btstack_classic_handle_hci_event_packet(uint8_t *packet, uint16_t size);

static void pbdrv_bluetooth_btstack_classic_handle_packet(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    switch (packet_type) {
        case HCI_EVENT_PACKET: {
            pbdrv_bluetooth_btstack_classic_handle_hci_event_packet(packet, size);
            break;
        }

        case RFCOMM_DATA_PACKET: {
            pbdrv_bluetooth_classic_rfcomm_socket_t *sock = pbdrv_bluetooth_classic_rfcomm_socket_find_by_cid(channel);

            if (!sock) {
                DEBUG_PRINT("Received RFCOMM data for unknown channel: 0x%04x\n", channel);
                break;
            }

            if (size > lwrb_get_free(&sock->rx_buffer)) {
                DEBUG_PRINT("Received RFCOMM data that exceeds buffer capacity: %u\n", size);
                sock->err = PBIO_ERROR_FAILED;
            }

            lwrb_write(&sock->rx_buffer, packet, size);

            // Each packet we receive consumed a credit on the remote side.
            --sock->credits_outstanding;

            // If we receive a tiny packet, maybe we can grant more credits.
            pbdrv_bluetooth_classic_rfcomm_socket_grant_owed_credits(sock);
            pbio_os_request_poll();
            break;
        }
    }
}

static bool hci_handle_to_bd_addr(uint16_t handle, bd_addr_t addr) {
    hci_connection_t *conn = hci_connection_for_handle(handle);
    if (!conn) {
        return false;
    }
    memcpy(addr, conn->address, sizeof(bd_addr_t));
    return true;
}

static void pbdrv_bluetooth_btstack_classic_handle_hci_event_packet(uint8_t *packet, uint16_t size) {
    switch (hci_event_packet_get_type(packet)) {
        case HCI_EVENT_COMMAND_COMPLETE: {
            const uint8_t *rp = hci_event_command_complete_get_return_parameters(packet);
            switch (hci_event_command_complete_get_command_opcode(packet)) {
                case HCI_OPCODE_HCI_READ_LOCAL_VERSION_INFORMATION: {
                    pbdrv_bluetooth_local_version_info_t *ret = pending_local_version_info_request;
                    if (!ret) {
                        return;
                    }
                    ret->status = rp[0];
                    ret->hci_version = rp[1];
                    ret->hci_revision = rp[2] | ((uint16_t)rp[3] << 8);
                    ret->lmp_pal_version = rp[4];
                    ret->manufacturer = rp[5] | ((uint16_t)rp[6] << 8);
                    ret->lmp_pal_subversion = rp[7] | ((uint16_t)rp[8] << 8);
                    pending_local_version_info_request = NULL;
                    pbio_os_request_poll();
                    return;
                }
                default:
                    break;
            }
            break;
        }
        case GAP_EVENT_INQUIRY_RESULT: {
            if (!pending_inquiry_result_handler) {
                return;
            }
            pbdrv_bluetooth_inquiry_result_t result;
            gap_event_inquiry_result_get_bd_addr(packet, result.bdaddr);
            if (gap_event_inquiry_result_get_rssi_available(packet)) {
                result.rssi = gap_event_inquiry_result_get_rssi(packet);
            }
            if (gap_event_inquiry_result_get_name_available(packet)) {
                const uint8_t *name = gap_event_inquiry_result_get_name(packet);
                const size_t name_len = gap_event_inquiry_result_get_name_len(packet);
                snprintf(result.name, sizeof(result.name), "%.*s", (int)name_len, name);
            }
            result.class_of_device = gap_event_inquiry_result_get_class_of_device(packet);
            pending_inquiry_result_handler(pending_inquiry_result_handler_context, &result);
            if (pending_inquiry_response_limit > 0) {
                pending_inquiry_response_count++;
                if (pending_inquiry_response_count >= pending_inquiry_response_limit) {
                    gap_inquiry_stop();
                }
            }
            break;
        }

        case GAP_EVENT_INQUIRY_COMPLETE: {
            if (pending_inquiry_result_handler) {
                pending_inquiry_result_handler = NULL;
                pending_inquiry_result_handler_context = NULL;
                pbio_os_request_poll();
            }
            break;
        }

        case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
            // Pairing handlers, in case auto-accept doesn't work.
            bd_addr_t requester_addr;
            hci_event_user_confirmation_request_get_bd_addr(packet, requester_addr);
            DEBUG_PRINT("SSP User Confirmation Request. Auto-accepting...\n");
            gap_ssp_confirmation_response(requester_addr);
            break;
        }

        case HCI_EVENT_PIN_CODE_REQUEST: {
            bd_addr_t requester_addr;
            hci_event_pin_code_request_get_bd_addr(packet, requester_addr);

            DEBUG_PRINT("LEGACY: PIN Request. Trying '0000'...\n");

            // 0000 is used as the default pin code for many devices. We'll try it, and if
            // we fail, we just won't be able to connect.
            gap_pin_code_response(requester_addr, "0000");
            break;
        }

        case HCI_EVENT_AUTHENTICATION_COMPLETE: {
            // If authentication fails, we may need to drop the link key from the database.
            uint8_t auth_status = hci_event_authentication_complete_get_status(packet);
            if (auth_status == ERROR_CODE_AUTHENTICATION_FAILURE || auth_status == ERROR_CODE_PIN_OR_KEY_MISSING) {
                DEBUG_PRINT("AUTH FAIL: Link key rejected/missing (Status 0x%02x).\n", auth_status);
                uint16_t handle = hci_event_authentication_complete_get_connection_handle(packet);
                bd_addr_t addr;
                if (!hci_handle_to_bd_addr(handle, addr)) {
                    DEBUG_PRINT("AUTH FAIL: Unknown address for handle 0x%04x\n", handle);
                    break;
                }

                gap_drop_link_key_for_bd_addr(addr);
                link_db_settings_save();
            }
            pbio_os_request_poll();
            break;
        }

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            // HCI disconnection events are our chance to check if we've failed to connect due to
            // some error in authentication, which might indicate that our link key database contains
            // some manner of stale key. This is not where we handle RFCOMM socket disconnections! See
            // below for the relevant RFCOMM events.
            uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
            uint16_t handle = hci_event_disconnection_complete_get_connection_handle(packet);
            bd_addr_t addr;
            if (reason == ERROR_CODE_AUTHENTICATION_FAILURE) { // Authentication Failure
                if (!hci_handle_to_bd_addr(handle, addr)) {
                    // Can't do anything without the address.
                    DEBUG_PRINT("DISCONNECTED: Unknown address for handle 0x%04x\n", handle);
                    break;
                }

                DEBUG_PRINT("DISCONNECTED: Bad Link Key.\n");
                gap_drop_link_key_for_bd_addr(addr);
                link_db_settings_save();
            } else {
                DEBUG_PRINT("DISCONNECTED: Reason 0x%02x\n", reason);
            }

            break;
        }

        case HCI_EVENT_LINK_KEY_NOTIFICATION: {
            // BTStack has already updated the link key database, so we just need to save it.
            DEBUG_PRINT("Link key updated, saving to settings.\n");
            link_db_settings_save();
            break;
        }

        case RFCOMM_EVENT_CHANNEL_OPENED: {
            // TODO: rescue the linked key for this address if we're above capacity in the saved settings buffer.
            uint8_t bdaddr[6];
            rfcomm_event_channel_opened_get_bd_addr(packet, bdaddr);
            uint16_t cid = rfcomm_event_channel_opened_get_rfcomm_cid(packet);
            pbdrv_bluetooth_classic_rfcomm_socket_t *sock = pbdrv_bluetooth_classic_rfcomm_socket_find_by_cid(cid);

            if (!sock) {
                // If we have no allocated socket for this cid, disconnect. It shouldn't
                // be possible for this to happen, but just in case . . .
                DEBUG_PRINT("Unknown cid (%u) associated with address %s\n",
                    cid, bd_addr_to_str(bdaddr));
                rfcomm_disconnect(cid);
                break;
            }
            int status = rfcomm_event_channel_opened_get_status(packet);
            if (status != 0) {
                DEBUG_PRINT("RFCOMM channel open failed with status: %d", status);
                sock->err = PBIO_ERROR_FAILED;
            } else {
                sock->connected = true;
                sock->mtu = rfcomm_event_channel_opened_get_max_frame_size(packet);
                sock->credits_outstanding = 0;
                pbdrv_bluetooth_classic_rfcomm_socket_grant_owed_credits(sock);
            }
            break;
        }

        case RFCOMM_EVENT_INCOMING_CONNECTION: {
            uint16_t cid = rfcomm_event_incoming_connection_get_rfcomm_cid(packet);

            if (!pending_listen_socket) {
                DEBUG_PRINT("Received unexpected incoming RFCOMM connection.\n");
                rfcomm_disconnect(cid);
                break;
            }
            pbdrv_bluetooth_classic_rfcomm_socket_t *sock = pending_listen_socket;
            pending_listen_socket = NULL;

            // Note: we aren't connected yet. We'll get an RFCOMM_EVENT_CHANNEL_OPENED for
            // this channel once we're officially online.
            rfcomm_accept_connection(cid);
            sock->cid = cid;

            // Mark ourselves as no longer connectable, since we aren't listening.
            gap_connectable_control(0);
            break;
        }

        case RFCOMM_EVENT_CAN_SEND_NOW: {
            uint16_t cid = rfcomm_event_can_send_now_get_rfcomm_cid(packet);
            pbdrv_bluetooth_classic_rfcomm_socket_t *sock = pbdrv_bluetooth_classic_rfcomm_socket_find_by_cid(cid);
            if (!sock) {
                DEBUG_PRINT("Unknown cid (%u) for CAN_SEND_NOW event, dropping connection.\n", cid);
                rfcomm_disconnect(cid);
                break;
            }

            if (lwrb_get_full(&sock->tx_buffer) == 0) {
                // Nothing to send. This is normal, since we get this event immediately when the
                // channel is opened, and the Python code won't usually have gotten around to giving
                // us a transmit buffer yet.
                break;
            }

            uint8_t *data = lwrb_get_linear_block_read_address(&sock->tx_buffer);
            uint16_t write_len = lwrb_get_linear_block_read_length(&sock->tx_buffer);


            if (write_len > sock->mtu) {
                write_len = sock->mtu;
            }

            int err = rfcomm_send(sock->cid, data, write_len);
            lwrb_skip(&sock->tx_buffer, write_len);
            if (err) {
                DEBUG_PRINT("Failed to send RFCOMM data: %d\n", err);
                sock->err = PBIO_ERROR_FAILED;
                rfcomm_disconnect(sock->cid);
                break;
            }


            if (lwrb_get_full(&sock->tx_buffer) == 0) {
                pbio_os_request_poll();
            } else {
                // If there's more data we need to do another send request.
                rfcomm_request_can_send_now_event(sock->cid);
            }

            break;
        }

        case RFCOMM_EVENT_CHANNEL_CLOSED: {
            uint16_t cid = rfcomm_event_channel_closed_get_rfcomm_cid(packet);
            pbdrv_bluetooth_classic_rfcomm_socket_t *sock = pbdrv_bluetooth_classic_rfcomm_socket_find_by_cid(cid);
            if (!sock) {
                DEBUG_PRINT("Unknown cid (%u) for CHANNEL_CLOSED event\n", cid);
                break;
            }
            // Note: we do not reset the socket, since the user is expected
            // to call pbdrv_bluetooth_rfcomm_close() on the connection first.

            if (sock->connected) {
                DEBUG_PRINT("RFCOMM channel (cid=%u) closed for unknown reason.\n", cid);
                sock->err = PBIO_ERROR_IO;
                sock->connected = false;
            }
            break;
        }

        default:
            break;
    }
}

// sdp_query_pending is true while some rfcomm_connect is using the sdp query
// mechanism. A new query can't be started while this is true.
static bool sdp_query_pending = false;
// sdp_query_channel_id_result is a pointer to where we should store the
// resulting rfcomm remote channel ID. We set this to NULL after we find a
// channel. This is used to indicate that the query is complete, but the
// caller is still responsible for setting sdp_query_pending to false.
static uint16_t *sdp_query_rfcomm_channel;

static void bluetooth_btstack_classic_sdp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }
    switch (hci_event_packet_get_type(packet)) {
        case SDP_EVENT_QUERY_RFCOMM_SERVICE: {
            if (!sdp_query_pending) {
                DEBUG_PRINT("Received unexpected SDP query result.\n");
                return;
            }
            if (*sdp_query_rfcomm_channel != 1) {
                // Note: we prefer to return channel 1 over any other channel, since this is the default
                // spp profile channel. The main purpose of this SDP query is to find channels *other* than
                // one, since the default channel may not be served especially in the case of Windows bluetooth
                // com ports.
                //
                // One limitation of our implementation here is that we don't provide the user any way to select
                // between multiple RFCOMM channels. Perhaps in the future we will allow users to manually specify
                // the channel if they know their server will be listening on a channel other than 1. All EV3s
                // will listen on channel 1.
                *sdp_query_rfcomm_channel = sdp_event_query_rfcomm_service_get_rfcomm_channel(packet);
            }
            DEBUG_PRINT("Found RFCOMM channel: %u\n", *sdp_query_rfcomm_channel);
            break;
        }
        case SDP_EVENT_QUERY_COMPLETE: {
            // Note: we don't indicate query completion to the caller until we get
            // this query complete signal. This is to prevent another query being
            // started while this query is ongoing.
            DEBUG_PRINT("SDP query complete.\n");
            sdp_query_rfcomm_channel = NULL;
            pbio_os_request_poll();
            break;
        }
        default: {
            DEBUG_PRINT("Received ignored SDP event: %u\n", packet_type);
            break;
        }
    }
}

/**
 * btstack's hci_power_control() synchronously emits an event that would cause
 * it to re-enter the event loop. This would not be safe to call from within
 * the event loop. This wrapper ensures it is called at most once.
 */
static pbio_error_t bluetooth_btstack_classic_handle_power_control(pbio_os_state_t *state, HCI_POWER_MODE power_mode, HCI_STATE end_state) {
    bool do_it_this_time = false;
    PBIO_OS_ASYNC_BEGIN(state);

    do_it_this_time = true;
    PBIO_OS_ASYNC_SET_CHECKPOINT(state);

    // The first time we get here, do_it_this_time = true, so we call
    // hci_power_control. When it re-enters at the checkpoint above, it will
    // be false, so move on.
    if (do_it_this_time) {
        hci_power_control(power_mode);
    }

    // Wait for the power state to take effect.
    PBIO_OS_AWAIT_UNTIL(state, hci_get_state() == end_state);

    PBIO_OS_ASYNC_END(PBIO_SUCCESS);
}

const char *pbdrv_bluetooth_get_hub_name(void) {
    // Not sure what this is for but the other bluetooth module implements it
    // and it referred to by the USB code?!
    return "Pybricks Hub";
}

// Reads the local version information from the Bluetooth controller.
pbio_error_t pbdrv_bluetooth_read_local_version_information(pbio_os_state_t *state, pbdrv_bluetooth_local_version_info_t *out) {
    PBIO_OS_ASYNC_BEGIN(state);
    PBIO_OS_AWAIT_UNTIL(state, hci_get_state() == HCI_STATE_WORKING);
    pending_local_version_info_request = out;
    hci_send_cmd(&hci_read_local_version_information);
    PBIO_OS_AWAIT_UNTIL(state, !pending_local_version_info_request);
    PBIO_OS_ASYNC_END(PBIO_SUCCESS);
}

pbio_error_t pbdrv_bluetooth_inquiry_scan(
    pbio_os_state_t *state,
    int32_t max_responses,
    int32_t timeout,
    void *context,
    pbdrv_bluetooth_inquiry_result_handler_t result_handler) {
    PBIO_OS_ASYNC_BEGIN(state);
    if (pending_inquiry_result_handler) {
        return PBIO_ERROR_BUSY;
    }
    PBIO_OS_AWAIT_UNTIL(state, hci_get_state() == HCI_STATE_WORKING);
    pending_inquiry_response_count = 0;
    pending_inquiry_response_limit = max_responses;
    pending_inquiry_result_handler = result_handler;
    pending_inquiry_result_handler_context = context;
    gap_inquiry_start(timeout);

    PBIO_OS_AWAIT_UNTIL(state, !pending_inquiry_result_handler);
    PBIO_OS_ASYNC_END(PBIO_SUCCESS);
}

void pbdrv_bluetooth_local_address(bdaddr_t addr) {
    gap_local_bd_addr(addr);
}

const char *pbdrv_bluetooth_bdaddr_to_str(const bdaddr_t addr) {
    return bd_addr_to_str(addr);
}

bool pbdrv_bluetooth_str_to_bdaddr(const char *str, bdaddr_t addr) {
    return sscanf_bd_addr(str, addr) == 1;
}

pbio_error_t pbdrv_bluetooth_rfcomm_connect(
    pbio_os_state_t *state,
    bdaddr_t bdaddr,
    int32_t timeout,
    pbdrv_bluetooth_rfcomm_conn_t *conn_out) {
    pbio_error_t err;

    // Each time we resume this function, we need to load the socket pointer.
    // On the first time through, this may find a spurious socket, but not to
    // worry, we reset the pointer in the first async stage before allocating
    // the socket.
    pbdrv_bluetooth_classic_rfcomm_socket_t *sock = pbdrv_bluetooth_classic_rfcomm_socket_find_by_conn(conn_out);

    PBIO_OS_ASYNC_BEGIN(state);

    sock = pbdrv_bluetooth_classic_rfcomm_socket_alloc();
    if (!sock) {
        DEBUG_PRINT("[btc:rfcomm_connect] No more sockets.\n");
        return PBIO_ERROR_RESOURCE_EXHAUSTED;
    }

    if (timeout > 0) {
        pbio_os_timer_set(&sock->tx_timer, timeout);
    }

    // Wait until the Bluetooth controller is up.
    PBIO_OS_AWAIT_UNTIL(state, hci_get_state() == HCI_STATE_WORKING || pbio_os_timer_is_expired(&sock->tx_timer));

    // Wait until any other pending SDP query is done.
    PBIO_OS_AWAIT_UNTIL(state, !sdp_query_pending || pbio_os_timer_is_expired(&sock->tx_timer));

    if (timeout > 0 && pbio_os_timer_is_expired(&sock->tx_timer)) {
        DEBUG_PRINT("[btc:rfcomm_connect] Timed out waiting for HCI_STATE_WORKING.\n");
        err = PBIO_ERROR_TIMEDOUT;
        goto cleanup;
    }

    conn_out->conn_id = pbdrv_bluetooth_classic_rfcomm_socket_id(sock);

    // TODO: allow manually specifying the channel if the user knows it already.
    DEBUG_PRINT("[btc:rfcomm_connect] Starting SDP query...\n");
    sdp_query_pending = true;

    // Note: valid server channels from 1-30, so this should never be returned
    // by a real SDP response.
#define SERVER_CHANNEL_UNSET ((uint16_t)-1)

    sock->server_channel = SERVER_CHANNEL_UNSET;
    sdp_query_rfcomm_channel = &sock->server_channel;
    uint8_t sdp_err = sdp_client_query_rfcomm_channel_and_name_for_service_class_uuid(
        bluetooth_btstack_classic_sdp_packet_handler, (uint8_t *)bdaddr, BLUETOOTH_SERVICE_CLASS_SERIAL_PORT);
    if (sdp_err != 0) {
        DEBUG_PRINT("[btc:rfcomm_connect] Failed to start SDP query: %d\n", sdp_err);
        sdp_query_pending = false;
        sdp_query_rfcomm_channel = NULL;
        err = PBIO_ERROR_FAILED;
        goto cleanup;
    }
    PBIO_OS_AWAIT_UNTIL(state, !sdp_query_rfcomm_channel);
    // Allow other SDP queries to go ahead.
    sdp_query_pending = false;

    if (sock->server_channel == SERVER_CHANNEL_UNSET) {
        DEBUG_PRINT("[btc:rfcomm_connect] Failed to find RFCOMM channel for device.\n");
        err = PBIO_ERROR_FAILED;
        goto cleanup;
    }
    DEBUG_PRINT("[btc:rfcomm_connect] Found RFCOMM channel %d for device.\n", sock->server_channel);

    // We establish the channel with no credits. Once we know the negotiated
    // MTU, we can calculate the number of credits we should grant.
    uint8_t rfcomm_err;
    if ((rfcomm_err = rfcomm_create_channel_with_initial_credits(&pbdrv_bluetooth_btstack_classic_handle_packet, (uint8_t *)bdaddr, sock->server_channel, 0, &sock->cid)) != 0) {
        DEBUG_PRINT("[btc:rfcomm_connect] Failed to create RFCOMM channel: %d\n", rfcomm_err);
        err = PBIO_ERROR_FAILED;
        goto cleanup;
    }
    PBIO_OS_AWAIT_UNTIL(state, sock->connected || sock->err != PBIO_SUCCESS || (timeout > 0 && pbio_os_timer_is_expired(&sock->tx_timer)));
    if (timeout > 0 && pbio_os_timer_is_expired(&sock->tx_timer)) {
        DEBUG_PRINT("[btc:rfcomm_connect] Timed out waiting for RFCOMM channel to connect.\n");
        err = PBIO_ERROR_TIMEDOUT;
        goto cleanup;
    }
    if (!sock->connected) {
        DEBUG_PRINT("[btc:rfcomm_connect] Other error.\n");
        err = sock->err;
        goto cleanup;
    }
    if (sock->mtu <= 0) {
        DEBUG_PRINT("[btc:rfcomm_connect] Failed to get MTU for RFCOMM channel, will not be able to send.\n");
        rfcomm_disconnect(sock->cid);
        err = PBIO_ERROR_FAILED;
        goto cleanup;
    }

    DEBUG_PRINT("[btc:rfcomm_connect] Connected (cid=%d remote=%s mtu=%d server_chan=%d)\n",
        sock->cid, bd_addr_to_str(bdaddr), sock->mtu, sock->server_channel);

    PBIO_OS_ASYNC_END(PBIO_SUCCESS);

cleanup:
    pbdrv_bluetooth_classic_rfcomm_socket_reset(sock);
    return err;
}

pbio_error_t pbdrv_bluetooth_rfcomm_listen(
    pbio_os_state_t *state,
    int32_t timeout,
    pbdrv_bluetooth_rfcomm_conn_t *conn) {
    pbio_error_t err;
    pbdrv_bluetooth_classic_rfcomm_socket_t *sock = pbdrv_bluetooth_classic_rfcomm_socket_find_by_conn(conn);

    PBIO_OS_ASYNC_BEGIN(state);
    sock = pbdrv_bluetooth_classic_rfcomm_socket_alloc();
    if (!sock) {
        DEBUG_PRINT("[btc:rfcomm_listen] No more sockets.\n");
        return PBIO_ERROR_RESOURCE_EXHAUSTED;
    }
    conn->conn_id = pbdrv_bluetooth_classic_rfcomm_socket_id(sock);

    if (timeout > 0) {
        // We use the rx timer to track listen timeouts, since we don't have
        // any other need for it until the connection is established.
        pbio_os_timer_set(&sock->rx_timer, timeout);
    }

    PBIO_OS_AWAIT_UNTIL(state, hci_get_state() == HCI_STATE_WORKING);
    if (timeout > 0 && pbio_os_timer_is_expired(&sock->rx_timer)) {
        DEBUG_PRINT("[btc:rfcomm_listen] Timed out waiting for HCI_STATE_WORKING.\n");
        err = PBIO_ERROR_TIMEDOUT;
        goto cleanup;
    }

    if (pending_listen_socket) {
        // Unlike with connect, where it's plausible for multiple async contexts
        // to be connecting to different devices, it's always going to be an
        // error to listen more than once at a time.
        DEBUG_PRINT("[btc:rfcomm_listen] Already listening.\n");
        err = PBIO_ERROR_BUSY;
        goto cleanup;
    }

    // Wait until either we time out, there is an error, or the socket is
    // connected.
    pending_listen_socket = sock;
    gap_connectable_control(1);
    PBIO_OS_AWAIT_UNTIL(state, sock->connected || sock->err != PBIO_SUCCESS || (timeout > 0 && pbio_os_timer_is_expired(&sock->rx_timer)));
    pending_listen_socket = NULL;

    if (timeout > 0 && pbio_os_timer_is_expired(&sock->rx_timer)) {
        // Note: if we timed out and the connection later establishes after we
        // free up the socket, it's fine, because we'll just disconnect
        // automatically in the packet handler.
        DEBUG_PRINT("[btc:rfcomm_listen] Timed out.\n");
        err = PBIO_ERROR_TIMEDOUT;
        goto cleanup;
    }
    if (sock->err != PBIO_SUCCESS) {
        DEBUG_PRINT("[btc:rfcomm_listen] Other error.\n");
        err = sock->err;
        goto cleanup;
    }

    PBIO_OS_ASYNC_END(PBIO_SUCCESS);

cleanup:
    pbdrv_bluetooth_classic_rfcomm_socket_reset(sock);
    return err;
}

pbio_error_t pbdrv_bluetooth_rfcomm_close(
    const pbdrv_bluetooth_rfcomm_conn_t *conn) {
    pbdrv_bluetooth_classic_rfcomm_socket_t *sock = pbdrv_bluetooth_classic_rfcomm_socket_find_by_conn(conn);
    if (!sock) {
        DEBUG_PRINT("[btc:rfcomm_close] Invalid CID: %d\n", conn->conn_id);
        return PBIO_ERROR_INVALID_OP;
    }
    gap_disconnect(sock->cid);
    pbdrv_bluetooth_classic_rfcomm_socket_reset(sock);
    return PBIO_SUCCESS;
}

pbio_error_t pbdrv_bluetooth_rfcomm_send(
    const pbdrv_bluetooth_rfcomm_conn_t *conn,
    const uint8_t *data,
    size_t length,
    size_t *bytes_sent) {
    pbdrv_bluetooth_classic_rfcomm_socket_t *sock = pbdrv_bluetooth_classic_rfcomm_socket_find_by_conn(conn);
    if (!sock || !sock->connected) {
        DEBUG_PRINT("[btc:rfcomm_send] Socket is not connected or does not exist.\n");
        return PBIO_ERROR_FAILED;
    }

    DEBUG_PRINT("[btc:rfcomm_send] Sending '%.*s' to RFCOMM channel.\n", (int)length, data);

    bool was_idle = lwrb_get_full(&sock->tx_buffer) == 0;
    *bytes_sent = lwrb_write(&sock->tx_buffer, data, length);
    if (was_idle && *bytes_sent > 0) {
        // If we were idle before, we need to request a send event to kick
        // things off.
        rfcomm_request_can_send_now_event(sock->cid);
    }

    return PBIO_SUCCESS;
}

pbio_error_t pbdrv_bluetooth_rfcomm_recv(
    const pbdrv_bluetooth_rfcomm_conn_t *conn,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *bytes_received) {
    pbdrv_bluetooth_classic_rfcomm_socket_t *sock = pbdrv_bluetooth_classic_rfcomm_socket_find_by_conn(conn);

    if (!sock || !sock->connected) {
        DEBUG_PRINT("[btc:rfcomm_recv] Socket is not connected or does not exist.\n");
        return PBIO_ERROR_FAILED;
    }

    *bytes_received = lwrb_read(&sock->rx_buffer, buffer, buffer_size);
    if (*bytes_received > 0) {
        // After reading data, we may have freed up enough space to grant some
        // credits back to our peer.
        pbdrv_bluetooth_classic_rfcomm_socket_grant_owed_credits(sock);
    }

    return PBIO_SUCCESS;
}

bool pbdrv_bluetooth_rfcomm_is_writeable(const pbdrv_bluetooth_rfcomm_conn_t *conn) {
    pbdrv_bluetooth_classic_rfcomm_socket_t *sock = pbdrv_bluetooth_classic_rfcomm_socket_find_by_conn(conn);
    if (!sock || !sock->connected) {
        return false;
    }
    return lwrb_get_free(&sock->tx_buffer) > 0;
}

bool pbdrv_bluetooth_rfcomm_is_readable(const pbdrv_bluetooth_rfcomm_conn_t *conn) {
    pbdrv_bluetooth_classic_rfcomm_socket_t *sock = pbdrv_bluetooth_classic_rfcomm_socket_find_by_conn(conn);
    if (!sock) {
        return false;
    }
    return lwrb_get_full(&sock->rx_buffer) > 0;
}

bool pbdrv_bluetooth_rfcomm_is_connected(const pbdrv_bluetooth_rfcomm_conn_t *conn) {
    pbdrv_bluetooth_classic_rfcomm_socket_t *sock = pbdrv_bluetooth_classic_rfcomm_socket_find_by_conn(conn);
    if (!sock) {
        return false;
    }
    return sock->connected;
}

static void bluetooth_btstack_classic_run_loop_init(void) {
    // Not used. Bluetooth process is started like a regular pbdrv process.
}

static btstack_linked_list_t data_sources;

static void bluetooth_btstack_classic_run_loop_add_data_source(btstack_data_source_t *ds) {
    btstack_linked_list_add(&data_sources, &ds->item);
}

static bool bluetooth_btstack_classic_run_loop_remove_data_source(btstack_data_source_t *ds) {
    return btstack_linked_list_remove(&data_sources, &ds->item);
}

static void bluetooth_btstack_classic_run_loop_enable_data_source_callbacks(btstack_data_source_t *ds, uint16_t callback_types) {
    ds->flags |= callback_types;
}

static void bluetooth_btstack_classic_run_loop_disable_data_source_callbacks(btstack_data_source_t *ds, uint16_t callback_types) {
    ds->flags &= ~callback_types;
}

static btstack_linked_list_t timers;

static void bluetooth_btstack_classic_run_loop_set_timer(btstack_timer_source_t *ts, uint32_t timeout_in_ms) {
    ts->timeout = pbdrv_clock_get_ms() + timeout_in_ms;
}

static void bluetooth_btstack_classic_run_loop_add_timer(btstack_timer_source_t *ts) {
    btstack_linked_item_t *it;
    for (it = (void *)&timers; it->next; it = it->next) {
        // don't add timer that's already in there
        btstack_timer_source_t *next = (void *)it->next;
        if (next == ts) {
            // timer was already in the list!
            // assert(0);
            return;
        }
        // exit if new timeout before list timeout
        int32_t delta = btstack_time_delta(ts->timeout, next->timeout);
        if (delta < 0) {
            break;
        }
    }

    ts->item.next = it->next;
    it->next = &ts->item;
}

static bool bluetooth_btstack_classic_run_loop_remove_timer(btstack_timer_source_t *ts) {
    if (btstack_linked_list_remove(&timers, &ts->item)) {
        return true;
    }
    return false;
}

static void bluetooth_btstack_classic_run_loop_execute(void) {
    // not used
}

static void bluetooth_btstack_classic_run_loop_dump_timer(void) {
    // not used
}

static const btstack_run_loop_t bluetooth_btstack_classic_run_loop = {
    .init = bluetooth_btstack_classic_run_loop_init,
    .add_data_source = bluetooth_btstack_classic_run_loop_add_data_source,
    .remove_data_source = bluetooth_btstack_classic_run_loop_remove_data_source,
    .enable_data_source_callbacks = bluetooth_btstack_classic_run_loop_enable_data_source_callbacks,
    .disable_data_source_callbacks = bluetooth_btstack_classic_run_loop_disable_data_source_callbacks,
    .set_timer = bluetooth_btstack_classic_run_loop_set_timer,
    .add_timer = bluetooth_btstack_classic_run_loop_add_timer,
    .remove_timer = bluetooth_btstack_classic_run_loop_remove_timer,
    .execute = bluetooth_btstack_classic_run_loop_execute,
    .dump_timer = bluetooth_btstack_classic_run_loop_dump_timer,
    .get_time_ms = pbdrv_clock_get_ms,
};

static bool do_poll_handler;

void pbdrv_bluetooth_btstack_classic_run_loop_trigger(void) {
    do_poll_handler = true;
    pbio_os_request_poll();
}

static pbio_os_process_t pbdrv_bluetooth_hci_process;

static pbio_os_state_t bluetooth_thread_state;
static pbio_os_state_t bluetooth_thread_err;
pbio_error_t pbdrv_bluetooth_process_thread(pbio_os_state_t *state, void *context);

/**
 * This process is slightly unusual in that it does not use its state. It is
 * essentially just a poll handler.
 */
static pbio_error_t pbdrv_bluetooth_hci_process_thread(pbio_os_state_t *state, void *context) {
    // Test only -- ensure the entire uart log is flushed before going forward.
    if (!pbdrv_uart_debug_is_done()) {
        pbio_os_request_poll();
        return PBIO_ERROR_AGAIN;
    }

    if (do_poll_handler) {
        do_poll_handler = false;

        btstack_data_source_t *ds, *next;
        for (ds = (void *)data_sources; ds != NULL; ds = next) {
            // cache pointer to next data_source to allow data source to remove itself
            next = (void *)ds->item.next;
            if (ds->flags & DATA_SOURCE_CALLBACK_POLL) {
                ds->process(ds, DATA_SOURCE_CALLBACK_POLL);
            }
        }
    }

    static pbio_os_timer_t btstack_timer = {
        .duration = 1,
    };

    if (pbio_os_timer_is_expired(&btstack_timer)) {
        pbio_os_timer_extend(&btstack_timer);

        // process all BTStack timers in list that have expired
        while (timers) {
            btstack_timer_source_t *ts = (void *)timers;
            int32_t delta = btstack_time_delta(ts->timeout, pbdrv_clock_get_ms());
            if (delta > 0) {
                // we have reached unexpired timers
                break;
            }
            bluetooth_btstack_classic_run_loop_remove_timer(ts);
            ts->process(ts);
        }
    }

    if (bluetooth_thread_err == PBIO_ERROR_AGAIN) {
        bluetooth_thread_err = pbdrv_bluetooth_process_thread(&bluetooth_thread_state, NULL);
    }

    return PBIO_ERROR_AGAIN;
}

void pbdrv_bluetooth_init(void) {
    DEBUG_PRINT("[btc] Starting classic BTStack ...\n");

    // Here we only initialize the HCI transport, btstack memory, and the run
    // loop, and kick off the hci process thread. The rest of initialization
    // moves forward in pbdrv_bluetooth_process_thread. Note that the bluetooth
    // thread is driven by the HCI thread.

    #if DEBUG
    hci_dump_init(&bluetooth_btstack_classic_hci_dump);
    bluetooth_btstack_classic_hci_dump_enable(false);
    #endif

    btstack_memory_init();
    btstack_run_loop_init(&bluetooth_btstack_classic_run_loop);
    hci_init(hci_transport_h4_instance_for_uart(pdata->uart_block_instance()), &hci_transport_config);
    hci_set_control(pdata->control_instance());
    bluetooth_thread_err = PBIO_ERROR_AGAIN;
    pbio_os_process_start(&pbdrv_bluetooth_hci_process, pbdrv_bluetooth_hci_process_thread, NULL);
}

static bool shutting_down = false;
static btstack_packet_callback_registration_t pbdrv_bluetooth_btstack_packet_handler_reg = {
    .item = { NULL },
    .callback = pbdrv_bluetooth_btstack_classic_handle_packet,
};

static pbio_error_t pbdrv_bluetooth_controller_reset(pbio_os_state_t *state, pbio_os_timer_t *timer) {
    static pbio_os_state_t sub;

    PBIO_OS_ASYNC_BEGIN(state);

    // TODO: Disconnect classic connection if connected.

    // Wait for power off.
    PBIO_OS_AWAIT(state, &sub, bluetooth_btstack_classic_handle_power_control(&sub, HCI_POWER_OFF, HCI_STATE_OFF));

    PBIO_OS_ASYNC_END(PBIO_SUCCESS);
}

static pbio_error_t pbdrv_bluetooth_controller_initialize(pbio_os_state_t *state, pbio_os_timer_t *timer) {
    static pbio_os_state_t sub;

    // The first time we init the classic chip, we don't know what
    // subversion the chip is (could be cc2560 or cc2560a). We must read the
    // subversion from the chip itself, then select the appropriate init script.
    static uint16_t lmp_subversion = 0;

    PBIO_OS_ASYNC_BEGIN(state);

    hci_add_event_handler(&pbdrv_bluetooth_btstack_packet_handler_reg);
    if (lmp_subversion == 0) {
        static pbdrv_bluetooth_local_version_info_t version_info;
        // We must power-on the bluetooth chip before
        DEBUG_PRINT("[btc] Powering on the chip to read version info...\n");
        PBIO_OS_AWAIT(state, &sub, bluetooth_btstack_classic_handle_power_control(&sub, HCI_POWER_ON, HCI_STATE_INITIALIZING));
        DEBUG_PRINT("[btc] Chip is on, reading version info...\n");
        PBIO_OS_AWAIT(state, &sub, pbdrv_bluetooth_read_local_version_information(&sub, &version_info));
        lmp_subversion = version_info.lmp_pal_subversion;
        DEBUG_PRINT("[btc] Detected LMP Subversion: 0x%04x\n", lmp_subversion);
        // Power down the chip -- we'll power it up again later with the correct init script.
        PBIO_OS_AWAIT(state, &sub, bluetooth_btstack_classic_handle_power_control(&sub, HCI_POWER_OFF, HCI_STATE_OFF));
        DEBUG_PRINT("Power is off\n", lmp_subversion);
    }

    pbdrv_bluetooth_init_script_t init_script;
    pbio_error_t err = pbdrv_bluetooth_get_init_script(lmp_subversion, &init_script);
    if (err != PBIO_SUCCESS) {
        DEBUG_PRINT("Unsupported LMP Subversion: 0x%04" PRIx16 "\n", lmp_subversion);
        return err;
    }
    btstack_chipset_cc256x_set_init_script((uint8_t *)init_script.script, init_script.script_size);
    hci_set_chipset(pdata->chipset_instance());

    l2cap_init();
    rfcomm_init();

    gap_ssp_set_enable(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_auto_accept(true);
    gap_set_class_of_device(0x000804);  // Toy : Robot

    hci_set_link_key_db(btstack_link_key_db_memory_instance());

    link_db_settings_load();

    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);
    sdp_client_init();
    rfcomm_set_required_security_level(LEVEL_2);

    // All EV3s listen only on channel 1 (the default SPP channel).
    rfcomm_register_service_with_initial_credits(
        &pbdrv_bluetooth_btstack_classic_handle_packet,
        1,
        1024,
        0);

    bluetooth_thread_err = PBIO_ERROR_AGAIN;

    // Wait for power on.
    PBIO_OS_AWAIT(state, &sub, bluetooth_btstack_classic_handle_power_control(&sub, HCI_POWER_ON, HCI_STATE_WORKING));

    gap_discoverable_control(0);
    gap_connectable_control(0);
    gap_set_bondable_mode(0);

    PBIO_OS_ASYNC_END(PBIO_SUCCESS);
}


pbio_error_t pbdrv_bluetooth_process_thread(pbio_os_state_t *state, void *context) {
    static pbio_os_state_t sub;
    static pbio_os_timer_t timer;

    PBIO_OS_ASYNC_BEGIN(state);

    DEBUG_PRINT("[btc] Reset controller\n");

    // Reset and initialize the controller.
    PBIO_OS_AWAIT(state, &sub, pbdrv_bluetooth_controller_reset(&sub, &timer));

    DEBUG_PRINT("[btc] Initialize controller\n");
    PBIO_OS_AWAIT(state, &sub, pbdrv_bluetooth_controller_initialize(&sub, &timer));

    DEBUG_PRINT("[btc] Controller initialized. Awaiting shutdown.\n");

    while (!shutting_down) {
        PBIO_OS_AWAIT_MS(state, &timer, 100);
    }

    PBIO_OS_AWAIT(state, &sub, pbdrv_bluetooth_controller_reset(&sub, &timer));
    hci_remove_event_handler(&pbdrv_bluetooth_btstack_packet_handler_reg);

    PBIO_OS_ASYNC_END(PBIO_SUCCESS);
}

void pbdrv_bluetooth_deinit(void) {
    shutting_down = true;
    pbio_os_request_poll();
}

#endif // PBDRV_CONFIG_BLUETOOTH_BTSTACK_CLASSIC
