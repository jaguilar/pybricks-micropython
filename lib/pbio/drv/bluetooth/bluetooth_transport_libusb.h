#ifndef PBIO_DRV_BLUETOOTH_BLUETOOTH_TRANSPORT_LIBUSB_H
#define PBIO_DRV_BLUETOOTH_BLUETOOTH_TRANSPORT_LIBUSB_H

#include <pbdrvconfig.h>

#if PBDRV_CONFIG_BLUETOOTH_TRANSPORT_LIBUSB

#include <btstack.h>

const hci_transport_t* pbdrv_bluetooth_transport_libusb_instance(void);

void pbdrv_bluetooth_libusb_chipset_detect_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size);

#endif  // PBDRV_CONFIG_BLUETOOTH_TRANSPORT_LIBUSB

#endif // PBIO_DRV_BLUETOOTH_BLUETOOTH_TRANSPORT_LIBUSB_H
