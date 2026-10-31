// Copyright 2022 Nick Brassel (@tzarc)
// SPDX-License-Identifier: GPL-2.0-or-later
#include <numeric>
#include "gtest/gtest.h"
#include "gmock/gmock.h"
#include "backing_mocks.hpp"

class WearLeveling8Byte : public ::testing::Test {
   protected:
    void SetUp() override {
        MockBackingStore::Instance().reset_instance();
        wear_leveling_init();
    }
};

static std::array<std::uint8_t, WEAR_LEVELING_LOGICAL_SIZE> verify_data;

/* ak820pro (the 10-31 audit, F-010): an 8-byte store's multi-byte line holds
 * 4 value bytes, and byte 7 is a Berger check, the count of 0-bits in bytes
 * 0-6 (wear_leveling.c's wl_line_check()), which a line built here sets. */
static void set_line_check(write_log_entry_t& e) {
    uint8_t ones = 0;
    for (int i = 0; i < 7; i++)
        for (uint8_t v = e.raw8[i]; v; v &= (uint8_t)(v - 1)) ones++;
    e.raw8[7] = (uint8_t)(56 - ones);
}

static wear_leveling_status_t test_write(const uint32_t address, const void* value, size_t length) {
    memcpy(&verify_data[address], value, length);
    return wear_leveling_write(address, value, length);
}

/**
 * This test verifies that the first write after initialisation occurs after the FNV1a_64 hash location.
 */
TEST_F(WearLeveling8Byte, FirstWriteOccursAfterHash) {
    auto&   inst       = MockBackingStore::Instance();
    uint8_t test_value = 0x15;
    test_write(0x02, &test_value, sizeof(test_value));
    EXPECT_EQ(inst.log_begin()->address, WEAR_LEVELING_LOGICAL_SIZE + 8) << "Invalid first write address.";
}

/**
 * This test verifies that the first write after initialisation occurs after the FNV1a_64 hash location, after an erase has occurred.
 */
TEST_F(WearLeveling8Byte, FirstWriteOccursAfterHash_AfterErase) {
    auto&   inst       = MockBackingStore::Instance();
    uint8_t test_value = 0x15;
    wear_leveling_erase();
    test_write(0x02, &test_value, sizeof(test_value));
    EXPECT_EQ((inst.log_begin() + 1)->address, WEAR_LEVELING_LOGICAL_SIZE + 8) << "Invalid first write address.";
}

/**
 * This test ensures the correct number of backing store writes occurs with a multibyte write, given the input buffer size.
 * ak820pro (F-010): 4 bytes a line, so 5 takes two.
 */
TEST_F(WearLeveling8Byte, MultibyteBackingStoreWriteCounts) {
    auto& inst = MockBackingStore::Instance();

    for (std::size_t length = 1; length <= 8; ++length) {
        // Clear things out
        std::fill(verify_data.begin(), verify_data.end(), 0);
        inst.reset_instance();
        wear_leveling_init();

        // Generate a test block of data
        std::vector<std::uint8_t> testvalue(length);
        std::iota(testvalue.begin(), testvalue.end(), 0x20);

        // Write the data
        EXPECT_EQ(test_write(0, testvalue.data(), testvalue.size()), WEAR_LEVELING_SUCCESS) << "Write failed with incorrect status";

        // Check that we got the expected number of write log entries
        EXPECT_EQ(std::distance(inst.log_begin(), inst.log_end()), (long)((length + 3) / 4)) << "length" << length;
    }
}

/**
 * This test forces consolidation by writing enough to the write log that it overflows, consolidating the data into the
 * base logical area.
 */
TEST_F(WearLeveling8Byte, ConsolidationOverflow) {
    auto& inst = MockBackingStore::Instance();

    // Generate a test block of data
    std::array<std::uint8_t, WEAR_LEVELING_LOGICAL_SIZE> testvalue;

    // Write the data
    std::iota(testvalue.begin(), testvalue.end(), 0x20);
    EXPECT_EQ(test_write(0, testvalue.data(), testvalue.size()), WEAR_LEVELING_CONSOLIDATED) << "Write returned incorrect status";
    uint8_t dummy = 0x40;
    EXPECT_EQ(test_write(0x04, &dummy, sizeof(dummy)), WEAR_LEVELING_SUCCESS) << "Write returned incorrect status";

    // Expected log (ak820pro, F-010: 4 bytes a line):
    // [0]: multibyte, 4 bytes, backing address 0x18, logical address 0x00
    // [1]: multibyte, 4 bytes, backing address 0x20, logical address 0x04
    // [2]: multibyte, 4 bytes, backing address 0x28, logical address 0x08, triggers consolidation
    // [3]: erase
    // [4]: consolidated data, backing address 0x00, logical address 0x00
    // [5]: consolidated data, backing address 0x08, logical address 0x08
    // [6]: FNV1a_64 result, backing address 0x10
    // [7]: multibyte, 1 byte,  backing address 0x18, logical address 0x04
    EXPECT_EQ(std::distance(inst.log_begin(), inst.log_end()), 8);

    // Verify the backing store writes for the write log
    std::size_t       index;
    write_log_entry_t e;
    for (index = 0; index < 3; ++index) {
        auto write_iter = inst.log_begin() + index;
        EXPECT_EQ(write_iter->address, WEAR_LEVELING_LOGICAL_SIZE + 8 + (index * BACKING_STORE_WRITE_SIZE)) << "Invalid write log address";

        write_log_entry_t e;
        e.raw64 = write_iter->value;
        EXPECT_EQ(LOG_ENTRY_GET_TYPE(e), LOG_ENTRY_TYPE_MULTIBYTE) << "Invalid write log entry type";
    }

    // Verify the backing store erase
    {
        index           = 3;
        auto write_iter = inst.log_begin() + index;
        e.raw64         = write_iter->value;
        EXPECT_TRUE(write_iter->erased) << "Backing store erase did not occur as required";
    }

    // Verify the backing store writes for consolidation
    for (index = 4; index < 6; ++index) {
        auto write_iter = inst.log_begin() + index;
        EXPECT_EQ(write_iter->address, (index - 4) * BACKING_STORE_WRITE_SIZE) << "Invalid write log entry address";
    }

    // Verify the FNV1a_64 write
    {
        EXPECT_EQ((inst.log_begin() + 6)->address, WEAR_LEVELING_LOGICAL_SIZE) << "Invalid write log address";
        e.raw64 = (inst.log_begin() + 6)->value;
        EXPECT_EQ(e.raw64, fnv_64a_buf(testvalue.data(), testvalue.size(), FNV1A_64_INIT)) << "Invalid checksum"; // Note that checksum is based on testvalue, as we overwrote one byte and need to consult the consolidated data, not the current
    }

    // Verify the final write
    EXPECT_EQ((inst.log_begin() + 7)->address, WEAR_LEVELING_LOGICAL_SIZE + 8) << "Invalid write log address";

    // Verify the data is what we expected
    std::array<std::uint8_t, WEAR_LEVELING_LOGICAL_SIZE> readback;
    EXPECT_EQ(wear_leveling_read(0, readback.data(), WEAR_LEVELING_LOGICAL_SIZE), WEAR_LEVELING_SUCCESS) << "Failed to read back the saved data";
    EXPECT_TRUE(memcmp(readback.data(), verify_data.data(), WEAR_LEVELING_LOGICAL_SIZE) == 0) << "Readback did not match";

    // Re-init and re-read, verifying the reload capability
    EXPECT_NE(wear_leveling_init(), WEAR_LEVELING_FAILED) << "Re-initialisation failed";
    EXPECT_EQ(wear_leveling_read(0, readback.data(), WEAR_LEVELING_LOGICAL_SIZE), WEAR_LEVELING_SUCCESS) << "Failed to read back the saved data";
    EXPECT_TRUE(memcmp(readback.data(), verify_data.data(), WEAR_LEVELING_LOGICAL_SIZE) == 0) << "Readback did not match";
}

/**
 * This test verifies multibyte readback gets canceled with an out-of-bounds address.
 */
TEST_F(WearLeveling8Byte, PlaybackReadbackMultibyte_OOB) {
    auto& inst     = MockBackingStore::Instance();
    auto  logstart = inst.storage_begin() + (WEAR_LEVELING_LOGICAL_SIZE / sizeof(backing_store_int_t));

    // Invalid FNV1a_64 hash
    (logstart + 0)->set(0);

    // Set up a 2-byte logical write of [0x11,0x12] at logical offset 0x01
    auto entry0    = LOG_ENTRY_MAKE_MULTIBYTE(0x01, 2);
    entry0.raw8[3] = 0x11;
    entry0.raw8[4] = 0x12;
    set_line_check(entry0);
    (logstart + 1)->set(~entry0.raw64);

    // Set up a 2-byte logical write of [0x13,0x14] at logical offset 0x1000 (out of bounds)
    auto entry1    = LOG_ENTRY_MAKE_MULTIBYTE(0x1000, 2);
    entry1.raw8[3] = 0x13;
    entry1.raw8[4] = 0x14;
    set_line_check(entry1);
    (logstart + 2)->set(~entry1.raw64);

    // Set up a 2-byte logical write of [0x15,0x16] at logical offset 0x10
    auto entry2    = LOG_ENTRY_MAKE_MULTIBYTE(0x01, 2);
    entry2.raw8[3] = 0x15;
    entry2.raw8[4] = 0x16;
    set_line_check(entry2);
    (logstart + 3)->set(~entry2.raw64);

    EXPECT_EQ(inst.erasure_count(), 0) << "Invalid initial erase count";
    EXPECT_EQ(wear_leveling_init(), WEAR_LEVELING_CONSOLIDATED) << "Readback should have failed and triggered consolidation";
    EXPECT_EQ(inst.erasure_count(), 1) << "Invalid final erase count";

    uint8_t buf[2];
    wear_leveling_read(0x01, buf, sizeof(buf));
    EXPECT_EQ(buf[0], 0x11) << "Readback should have maintained the previous pre-failure value from the write log";
    EXPECT_EQ(buf[1], 0x12) << "Readback should have maintained the previous pre-failure value from the write log";
}

/**
 * ak820pro (the 10-31 audit, F-010): a torn line, some of its 1-bits never programmed, fails its check; playback stops
 * there and consolidates, keeping the writes before it. The tear here leaves a valid-looking write to a lower address:
 * [0x13,0x14] meant for 0x09 reads as a write to 0x01 once bit 3 of the address is lost.
 */
TEST_F(WearLeveling8Byte, PlaybackTornLine) {
    auto& inst     = MockBackingStore::Instance();
    auto  logstart = inst.storage_begin() + (WEAR_LEVELING_LOGICAL_SIZE / sizeof(backing_store_int_t));

    (logstart + 0)->set(0);

    auto entry0    = LOG_ENTRY_MAKE_MULTIBYTE(0x05, 2);
    entry0.raw8[3] = 0x11;
    entry0.raw8[4] = 0x12;
    set_line_check(entry0);
    (logstart + 1)->set(~entry0.raw64);

    auto entry1    = LOG_ENTRY_MAKE_MULTIBYTE(0x09, 2);
    entry1.raw8[3] = 0x13;
    entry1.raw8[4] = 0x14;
    set_line_check(entry1);
    write_log_entry_t torn = entry1;
    torn.raw8[2] &= (uint8_t)~0x08; // the address's bit 3: 0x09 -> 0x01
    EXPECT_EQ(LOG_ENTRY_MULTIBYTE_GET_ADDRESS(torn), 0x01u) << "the tear's shape";
    (logstart + 2)->set(~torn.raw64);

    EXPECT_EQ(wear_leveling_init(), WEAR_LEVELING_CONSOLIDATED) << "The torn line should have failed and triggered consolidation";

    uint8_t buf[2];
    wear_leveling_read(0x05, buf, sizeof(buf));
    EXPECT_EQ(buf[0], 0x11) << "The write before the torn line should have been kept";
    EXPECT_EQ(buf[1], 0x12) << "The write before the torn line should have been kept";
    wear_leveling_read(0x01, buf, sizeof(buf));
    EXPECT_EQ(buf[0], 0x00) << "The torn line should not have been replayed at its lower address";
    EXPECT_EQ(buf[1], 0x00) << "The torn line should not have been replayed at its lower address";
}
