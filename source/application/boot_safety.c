/*
 * This file is a part of: https://github.com/pkrumpl/frame-codebase
 *
 * Authored by: pkrumpl
 *
 * This is an additional file checking if the device is stuck in a boot loop
 * and if so, forces it to enter DFU mode for recovery.
 */

#include "boot_safety.h"
#include <stdbool.h>
#include "error_logging.h"
#include "nrf_soc.h"
#include "nrf52840.h"
#include "nrfx_log.h"

// GPREGRET register indices
#define GPREGRET_DFU_FLAG_MASK 0xFF     // Only first 8 bit for magic value
#define GPREGRET_BOOT_COUNTER_MASK 0xF0 // Next 8 bits for boot counter
#define GPREGRET_BOOT_COUNTER_SHIFT 4

// DFU flag value recognized by bootloader
#define DFU_FLAG_VALUE 0xB1

int boot_safety_init(void)
{
    uint32_t dfu_flag = NRF_POWER->GPREGRET & GPREGRET_DFU_FLAG_MASK;
    uint32_t boot_counter = (NRF_POWER->GPREGRET & GPREGRET_BOOT_COUNTER_MASK) >> GPREGRET_BOOT_COUNTER_SHIFT;

    // If DFU flag is already set (e.g., from frame.update()), this means that a DFU has been performed
    // This will only be true on fake hardware when debugging, otherwise the flag is reset by the bootloader on successful boot
    // So in this case, we can reset the register
    if (dfu_flag == DFU_FLAG_VALUE)
    {
        NRF_POWER->GPREGRET = NRF_POWER->GPREGRET & ~GPREGRET_DFU_FLAG_MASK;
        boot_counter = 0;
    }

    // Check if we've exceeded the maximum boot attempts
    if (boot_counter >= MAX_BOOT_ATTEMPTS)
    {
        // Enter emergency DFU mode
        // Set the DFU flag that the bootloader recognizes
        NRF_POWER->GPREGRET = (NRF_POWER->GPREGRET & ~GPREGRET_DFU_FLAG_MASK) | DFU_FLAG_VALUE;

        // Reset the device - bootloader will see the DFU flag and enter DFU mode
        NVIC_SystemReset();

        // Should never reach here
        return 0;
    }

    // Increment boot counter
    boot_counter++;

    // Save the incremented boot counter
    NRF_POWER->GPREGRET = (NRF_POWER->GPREGRET & ~GPREGRET_BOOT_COUNTER_MASK) | ((boot_counter & 0xFF) << GPREGRET_BOOT_COUNTER_SHIFT);

    return 0;
}

void boot_safety_clear_counter(void)
{
    // Clear the boot counter to indicate successful operation
    // Ignore errors here since this is a best-effort cleanup
    NRF_POWER->GPREGRET = NRF_POWER->GPREGRET & ~GPREGRET_BOOT_COUNTER_MASK;
}
