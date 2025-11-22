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

typedef uint8_t bdaddr_t[6];

// A single result from an inquiry scan.
typedef struct {
    bdaddr_t bdaddr;
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

/**
 * Copies the local bluetooth address into addr.
 *
 * @param [out] addr  Buffer to store the local bluetooth address.
 */
void pbdrv_bluetooth_local_address(bdaddr_t addr);

/**
 * Converts a Bluetooth address to a string.
 *
 * The return value may be overwritten by subsequent calls to this function.
 *
 * @param [in] addr  Bluetooth address to convert.
 * @return            String representation of the Bluetooth address.
 */
const char *pbdrv_bluetooth_bdaddr_to_str(const bdaddr_t addr);

/**
 * Converts a string to a Bluetooth address.
 *
 * The string should contain six hex bytes, represented as ascii,
 * separated by :, -, or space.
 *
 * @param [in] str   String representation of the Bluetooth address.
 * @param [out] addr Buffer to store the Bluetooth address.
 * @returns whether the address was parsed successfully.
 */
bool pbdrv_bluetooth_str_to_bdaddr(const char *str, bdaddr_t addr);

// Represents an rfcomm connection.
typedef struct {
    int8_t conn_id;
} pbdrv_bluetooth_rfcomm_conn_t;

/**
 * Connects to a Bluetooth Classic device using RFCOMM.
 *
 * @param [in]  state       Protothread state.
 * @param [in]  bdaddr      Bluetooth address of the device to connect to.
 * @param [in]  timeout     Timeout in milliseconds. Use 0 for no timeout.
 * @param [out] conn        On success, will be populated with connection information.
 * @return                  PBIO_ERROR_SUCCESS on success, or an error code on failure.
 */
pbio_error_t pbdrv_bluetooth_rfcomm_connect(
    pbio_os_state_t *state,
    bdaddr_t bdaddr,
    int32_t timeout,
    pbdrv_bluetooth_rfcomm_conn_t *conn);

/**
 * Listens for incoming Bluetooth Classic RFCOMM connections.
 *
 * @param [in]  state       Protothread state.
 * @param [in]  timeout     Timeout in milliseconds. Use 0 for no timeout
 * @param [out] conn        On success, will be populated with connection information.
 * @return                  PBIO_ERROR_SUCCESS on success, or an error code on failure.
 */
pbio_error_t pbdrv_bluetooth_rfcomm_listen(
    pbio_os_state_t *state,
    int32_t timeout,
    pbdrv_bluetooth_rfcomm_conn_t *conn);

/**
 * Closes an RFCOMM connection.
 *
 * Returns immediately, although the connection's resources may not be released
 * until the next Bluetooth event loop iteration or two.
 */
pbio_error_t pbdrv_bluetooth_rfcomm_close(
    const pbdrv_bluetooth_rfcomm_conn_t *conn);

/**
 * Requests that some data be sent over an RFCOMM connection.
 *
 * @param [in] state       Protothread state.
 * @param [in] conn        The RFCOMM connection to send data over.
 * @param [in] data        The data to send.
 * @param [in] length      The length of the data to send.
 * @param [out] sent_length Populated with number of bytes sent.
 * @returns
 *    PBIO_SUCCESS if the socket is connected, regardless of whether
 *        any data fit in the send buffer.
 *    PBIO_ERROR_FAILED if the socket is not connected.
 */
pbio_error_t pbdrv_bluetooth_rfcomm_send(
    const pbdrv_bluetooth_rfcomm_conn_t *conn,
    const uint8_t *data,
    size_t length,
    size_t *sent_length);

/**
 * Receives data over an RFCOMM connection.
 *
 * Does not block. Copies as many bytes as are available, up to the size of the
 * buffer, into buffer. Writes the number of bytes copied into received_length.
 *
 * @param [in] conn            The RFCOMM connection to receive data from.
 * @param [out] buffer         Buffer to store received data.
 * @param [in] buffer_length   The length of the buffer.
 * @param [out] received_length On success, will be populated with the number of bytes received.
 * @returns
 *      PBIO_SUCCESS if the socket is connected, OR if data was read (e.g.
 *          if the socket had been closed but there was still data in the
 *          receive buffer).
 *      PBIO_ERROR_FAILED if the socket is not connected.
 */
pbio_error_t pbdrv_bluetooth_rfcomm_recv(
    const pbdrv_bluetooth_rfcomm_conn_t *conn,
    uint8_t *buffer,
    size_t buffer_length,
    size_t *received_length);

/**
 * Returns whether there is space available in the send buffer, AND the socket
 * is connected.
 */
bool pbdrv_bluetooth_rfcomm_is_writeable(const pbdrv_bluetooth_rfcomm_conn_t *conn);

/**
 * Returns whether there is data in the receive buffer.
 */
bool pbdrv_bluetooth_rfcomm_is_readable(const pbdrv_bluetooth_rfcomm_conn_t *conn);

/**
 * Returns whether the socket is connected.
 */
bool pbdrv_bluetooth_rfcomm_is_connected(const pbdrv_bluetooth_rfcomm_conn_t *conn);

#endif  // PBDRV_CONFIG_BLUETOOTH_CLASSIC

#endif
