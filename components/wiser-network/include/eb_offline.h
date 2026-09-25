#ifndef EB_OFFLINE_H
#define EB_OFFLINE_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Initialize the Offline Control (mDNS + WSS)
 */
void eb_offline_init(void);

/**
 * @brief Send a command to a specific offline switch or all switches
 *
 * @param switch_index 1-based index (1 = switch1, 2 = switch2...). 0 = ALL
 * switches.
 * @param state true for ON, false for OFF
 */
void eb_offline_send_cmd(int switch_index, bool state);

/**
 * @brief Send a command to a specific device by device_id and ctl_type
 *
 * This is the preferred API when a device is known from the
 * eb_device_registry. The implementation will:
 * - Discover switches via mDNS
 * - Match the discovered device_id against the provided device_id
 * - Send a SwitchEvent JSON payload over WSS with the given ctl_type/state.
 *
 * @param device_id Device identifier from the registry JSON
 * @param ctl_type  Gang number / control type from registry
 * @param state     true = "on", false = "off"
 */
void eb_offline_send_device_cmd(const char *device_id, uint8_t ctl_type,
                                bool state);

#endif
