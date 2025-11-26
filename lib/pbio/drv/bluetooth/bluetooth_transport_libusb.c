#include "bluetooth_transport_libusb.h"

#include <pbdrvconfig.h>

#if PBDRV_CONFIG_BLUETOOTH_TRANSPORT_LIBUSB

#include <stdio.h>
#include <btstack.h>
#include <hci_transport_usb.h>
#include <stdlib.h>
#include <string.h>
#include "btstack_chipset_bcm.h"
#include "btstack_chipset_intel_firmware.h"
#include "btstack_chipset_realtek.h"

// Broadcom USB device name lookup table
// Maps USB vendor:product ID to BCM device name for firmware file selection
typedef struct {
    uint16_t vendor_id;
    uint16_t product_id;
    const char* device_name;
} bcm_device_entry_t;

static const bcm_device_entry_t bcm_device_table[] = {
    // Broadcom vendor ID (0x0a5c)
    { 0x0a5c, 0x21e8, "BCM20702A0" },  // DeLOCK Bluetooth 4.0
    { 0x0a5c, 0x22be, "BCM20702B0" },  // Generic USB Detuned Class 1
    { 0x0a5c, 0x21e1, "BCM20702A0" },  // HP Bluetooth module
    { 0x0a5c, 0x21e3, "BCM20702A0" },  // Broadcom BCM20702A0
    { 0x0a5c, 0x21e6, "BCM20702A1" },  // ThinkPad Bluetooth 4.0
    { 0x0a5c, 0x21f1, "BCM20702A0" },  // HP Bluetooth module
    { 0x0a5c, 0x21fb, "BCM20702A0" },  // Broadcom BCM20702A0
    { 0x0a5c, 0x21fd, "BCM20702A0" },  // Broadcom BCM20702A0
    { 0x0a5c, 0x640b, "BCM20703A1" },  // Broadcom BCM20703A1
    { 0x0a5c, 0x6410, "BCM20703A1" },  // Broadcom BCM20703A1

    // ASUS vendor ID (0x0b05) - uses Broadcom chips
    { 0x0b05, 0x17cb, "BCM20702A0" },  // ASUS BT400
    { 0x0b05, 0x17cf, "BCM20702A0" },  // ASUS USB-BT400

    // Dell vendor ID (0x413c) - uses Broadcom chips
    { 0x413c, 0x8143, "BCM20702A0" },  // Dell Wireless 365 Bluetooth
    { 0x413c, 0x8197, "BCM20702A0" },  // Dell DW380 Bluetooth

    // Apple vendor ID (0x05ac) - uses Broadcom chips
    { 0x05ac, 0x828d, "BCM43142A0" },  // Apple Bluetooth USB Host Controller
    { 0x05ac, 0x8286, "BCM43142A0" },  // Apple Bluetooth USB Host Controller
};

static const char* get_bcm_device_name(uint16_t vendor_id, uint16_t product_id) {
    for (size_t i = 0; i < sizeof(bcm_device_table) / sizeof(bcm_device_table[0]); i++) {
        if (bcm_device_table[i].vendor_id == vendor_id &&
            bcm_device_table[i].product_id == product_id) {
            return bcm_device_table[i].device_name;
        }
    }
    return NULL;
}

// Get the firmware directory path, checking environment variable first
static const char *get_firmware_path(void) {
    static char path_buffer[256];
    static bool initialized = false;
    
    if (initialized) {
        return path_buffer;
    }
    initialized = true;
    
    // Check for environment variable override
    const char* env_path = getenv("PYBRICKS_VIRTUALHUB_FIRMWARE_DIR");
    if (env_path != NULL && env_path[0] != '\0') {
        strncpy(path_buffer, env_path, sizeof(path_buffer) - 1);
        path_buffer[sizeof(path_buffer) - 1] = '\0';
        return path_buffer;
    }
    
    // Default to ~/.cache/pybricks/virtualhub/bt_firmware
    const char *home = getenv("HOME");
    if (home != NULL) {
        snprintf(path_buffer, sizeof(path_buffer), "%s/.cache/pybricks/virtualhub/bt_firmware", home);
    } else {
        // Fallback if HOME is not set
        strncpy(path_buffer, ".cache/pybricks/virtualhub/bt_firmware", sizeof(path_buffer) - 1);
        path_buffer[sizeof(path_buffer) - 1] = '\0';
    }
    
    return path_buffer;
}


// Pybricks only uses USB transport for virtualhub Bluetooth testing.
// In this situation, we don't have a single chipset that we can statically
// configure, because people might have different Bluetooth dongles.
//
// We provide a tool in tools/collect_bt_patches.py that collects firmware
// files for various common Bluetooth dongles into a single directory that
// BTStack can use to load the appropriate firmware at runtime.
// Here, we configure a packet handler that listens for the local version
// information from btstack to configure the correct chipset dynamically.
//
// Note that we don't have firmware for every possible type of bluetooth
// dongle. Broadcom, Intel, and Realtek comprise a large (>90%) majority
// of all available Bluetooth dongles. Currently only these chipsets are
// handled. If you have another chipset in your dongle, you can add
// support for it here.
 void pbdrv_bluetooth_libusb_chipset_detect_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    (void)channel;
    (void)size;

    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    static bool first = true;
    static uint16_t usb_vendor_id = 0;
    static uint16_t usb_product_id = 0;

    if (first) {
        first = false;

        // The realtek controllers need to be registered manually.
        uint16_t realtek_num_controllers = btstack_chipset_realtek_get_num_usb_controllers();
        uint16_t i;
        for (i = 0;i < realtek_num_controllers;i++) {
            uint16_t vendor_id;
            uint16_t product_id;
            btstack_chipset_realtek_get_vendor_product_id(i, &vendor_id, &product_id);
            hci_transport_usb_add_device(vendor_id, product_id);
        }
    }

    uint8_t event_type = hci_event_packet_get_type(packet);

    switch (event_type) {
        case HCI_EVENT_TRANSPORT_USB_INFO: {
            // Store USB vendor and product IDs for later use
            usb_vendor_id = hci_event_transport_usb_info_get_vendor_id(packet);
            usb_product_id = hci_event_transport_usb_info_get_product_id(packet);

            // Extract USB product ID for Realtek chipset configuration
            btstack_chipset_realtek_set_product_id(usb_product_id);
            break;
        }
        case HCI_EVENT_COMMAND_COMPLETE: {
            uint16_t opcode = hci_event_command_complete_get_command_opcode(packet);
            
            if (opcode == HCI_OPCODE_HCI_READ_LOCAL_VERSION_INFORMATION) {
                // Extract local version information
                const uint8_t *params = hci_event_command_complete_get_return_parameters(packet);
                uint16_t manufacturer = little_endian_read_16(params, 5);
                uint16_t lmp_subversion = little_endian_read_16(params, 7);
                
                const char *firmware_path = get_firmware_path();
                
                // Configure chipset based on Bluetooth manufacturer ID
                switch (manufacturer) {
                    case BLUETOOTH_COMPANY_ID_INTEL_CORP:
                        btstack_chipset_intel_set_firmware_path(firmware_path);
                        // Note: Intel uses firmware download mechanism, not chipset driver
                        break;
                    case BLUETOOTH_COMPANY_ID_REALTEK_SEMICONDUCTOR_CORPORATION:
                        btstack_chipset_realtek_set_lmp_subversion(lmp_subversion);
                        btstack_chipset_realtek_set_firmware_folder_path(firmware_path);
                        btstack_chipset_realtek_set_config_folder_path(firmware_path);
                        hci_set_chipset(btstack_chipset_realtek_instance());
                        break;
                    case BLUETOOTH_COMPANY_ID_BROADCOM_CORPORATION: {
                        // Look up BCM device name from USB vendor:product ID
                        btstack_chipset_bcm_set_hcd_folder_path(firmware_path);
                        const char* bcm_device_name = get_bcm_device_name(usb_vendor_id, usb_product_id);
                        if (bcm_device_name) {
                            printf("Detected Broadcom device: %s (USB %04x:%04x)\n",
                                bcm_device_name, usb_vendor_id, usb_product_id);
                            btstack_chipset_bcm_set_device_name(bcm_device_name);
                        }

                        hci_set_chipset(btstack_chipset_bcm_instance());
                        break;
                    }
                    default:
                        // Unknown manufacturer - no chipset-specific initialization needed
                        break;
                }
            }
            break;
        }
        default:
            break;
    }
}

const hci_transport_t* pbdrv_bluetooth_transport_libusb_instance(void) {
    return hci_transport_usb_instance();
}

#endif
