/*
 * Boot Safety Module
 *
 * Implements automatic DFU mode entry after consecutive resets to prevent
 * device bricking from persistent firmware errors.
 *
 * Mechanism:
 * - Tracks consecutive resets using upper 4 bits of GPREGRET register (8 bit)
 * - After MAX_BOOT_ATTEMPTS resets, automatically enters DFU mode
 */

#ifndef BOOT_SAFETY_H
#define BOOT_SAFETY_H

#include <stdint.h>

// Maximum number of consecutive resets before entering emergency DFU mode (should not be more than 10!)
#define MAX_BOOT_ATTEMPTS 10

/*
 * Enter emergency DFU mode by setting the DFU flag and resetting the device via soft reset.
 */
void enter_dfu_mode(void);

/*
 * Initialize boot safety mechanism.
 *
 * This function should be called early in main() after SoftDevice initialization.
 * It will:
 * 1. Read the boot counter from GPREGRET
 * 2. Increment the counter
 * 3. If counter >= MAX_BOOT_ATTEMPTS, enter emergency DFU mode
 * 4. Otherwise, save the incremented counter
 *
 * Returns: 0 on success, error code on failure
 */
int boot_safety_init(void);

/*
 * Clear the boot counter after successful operation.
 */
void boot_safety_clear_counter(void);

#endif // BOOT_SAFETY_H
