// Copyright 2022 Nick Brassel (@tzarc)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>
#include <stdlib.h>

/**
 * @typedef Status returned from any wear-leveling API.
 */
typedef enum wear_leveling_status_t {
    WEAR_LEVELING_FAILED,      //< Invocation failed
    WEAR_LEVELING_SUCCESS,     //< Invocation succeeded
    WEAR_LEVELING_CONSOLIDATED //< Invocation succeeded, consolidation occurred
} wear_leveling_status_t;

/**
 * Wear-leveling initialization
 *
 * @return Status of the request
 */
wear_leveling_status_t wear_leveling_init(void);

/**
 * Wear-leveling erasure.
 *
 * Clears the wear-leveling area, with the definition that the "reset state" of all data is zero.
 *
 * @return Status of the request
 */
wear_leveling_status_t wear_leveling_erase(void);

/**
 * Writes logical data into the backing store.
 *
 * Skips writes if there are no changes to written values. The entire written block is considered when attempting to
 * determine if an overwrite should occur -- if there is any data mismatch the entire block will be written to the log,
 * not just the changed bytes.
 *
 * @param address[in] the logical address to write data
 * @param value[in] pointer to the source buffer
 * @param length[in] length of the data
 * @return Status of the request
 */
wear_leveling_status_t wear_leveling_write(uint32_t address, const void* value, size_t length);

/**
 * ak820pro (2026-10-27): a consolidation now, as a full write log forces one:
 * the backing store erased and the cache written back. The board's
 * flash_tidy.c runs it early, where the LEDs' blink can't be seen, and a dev
 * build's write test (the configuration menu's) on demand. Additive: nothing
 * else calls it.
 */
wear_leveling_status_t wear_leveling_consolidate(void);

/**
 * ak820pro (2026-10-27): the bytes of the write log in use, 0 just after a
 * consolidation; the log fills at WEAR_LEVELING_BACKING_SIZE -
 * WEAR_LEVELING_LOGICAL_SIZE - 8, and the write that fills it consolidates.
 */
uint32_t wear_leveling_log_used(void);

/**
 * Reads logical data from the cache.
 *
 * @param address[in] the logical address to read data
 * @param value[out] pointer to the destination buffer
 * @param length[in] length of the data
 * @return Status of the request
 */
wear_leveling_status_t wear_leveling_read(uint32_t address, void* value, size_t length);
