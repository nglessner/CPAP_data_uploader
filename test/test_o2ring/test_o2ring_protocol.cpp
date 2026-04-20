#include <unity.h>
#include <cstring>
#include <cstdint>

#include "../mocks/Arduino.cpp"
#include "../mocks/MockLogger.h"
#define LOGGER_H

#include "O2RingProtocol.h"

void setUp(void) {}
void tearDown(void) {}

void test_crc8_empty_input() {
    uint8_t result = O2RingProtocol::crc8(nullptr, 0);
    TEST_ASSERT_EQUAL_UINT8(0, result);
}

void test_crc8_single_byte() {
    uint8_t b = 0xAA;
    uint8_t result = O2RingProtocol::crc8(&b, 1);
    // 0xAA = 10101010; bits 1,3,5,7 set
    // chk = 0 ^ 0xAA = 0xAA
    // bit1(0x02): crc ^= 0x0e → 0x0e
    // bit3(0x08): crc ^= 0x38 → 0x36
    // bit5(0x20): crc ^= 0xe0 → 0xD6
    // bit7(0x80): crc ^= 0x89 → 0x5F
    TEST_ASSERT_EQUAL_UINT8(0x5F, result);
}

void test_build_info_packet_structure() {
    uint8_t buf[64];
    size_t len = O2RingProtocol::buildPacket(buf, O2RingProtocol::CMD_INFO, 0, nullptr, 0);
    TEST_ASSERT_EQUAL_size_t(8, len);
    TEST_ASSERT_EQUAL_UINT8(0xAA, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x14, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0xEB, buf[2]); // 0x14 ^ 0xFF
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[3]); // block lo
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[4]); // block hi
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[5]); // len lo
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[6]); // len hi
    // buf[7] = CRC of bytes 0..6
    uint8_t expected_crc = O2RingProtocol::crc8(buf, 7);
    TEST_ASSERT_EQUAL_UINT8(expected_crc, buf[7]);
}

void test_build_file_open_packet() {
    const char* filename = "20260116233312.vld";
    uint16_t dataLen = (uint16_t)(strlen(filename) + 1); // +1 for null terminator
    uint8_t buf[64];
    size_t len = O2RingProtocol::buildPacket(buf, O2RingProtocol::CMD_FILE_OPEN, 0,
                                              (const uint8_t*)filename, dataLen);
    TEST_ASSERT_EQUAL_size_t(7 + dataLen + 1, len);
    TEST_ASSERT_EQUAL_UINT8(0x03, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0xFC, buf[2]); // 0x03 ^ 0xFF
    TEST_ASSERT_EQUAL_UINT8((uint8_t)dataLen, buf[5]);
    TEST_ASSERT_EQUAL_UINT8(0, buf[6]);
    TEST_ASSERT_EQUAL_UINT8('2', buf[7]);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[7 + dataLen - 1]); // null terminator
}

void test_build_file_read_block_encoding() {
    uint8_t buf[16];
    // Block 3 (little-endian: lo=3, hi=0)
    O2RingProtocol::buildPacket(buf, O2RingProtocol::CMD_FILE_READ, 3, nullptr, 0);
    TEST_ASSERT_EQUAL_UINT8(0x04, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0x03, buf[3]); // block lo
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[4]); // block hi
    // Block 256 (lo=0, hi=1)
    O2RingProtocol::buildPacket(buf, O2RingProtocol::CMD_FILE_READ, 256, nullptr, 0);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[3]); // block lo
    TEST_ASSERT_EQUAL_UINT8(0x01, buf[4]); // block hi
}

void test_expected_response_len() {
    // Simulate header bytes with data_length = 20
    uint8_t header[7] = {0xAA, 0x14, 0xEB, 0x00, 0x00, 0x14, 0x00};
    size_t expected = O2RingProtocol::expectedResponseLen(header);
    TEST_ASSERT_EQUAL_size_t(7 + 20 + 1, expected); // 28
}

void test_parse_info_filelist_two_files() {
    const char* json = "{\"CurBAT\":\"75%\",\"FileList\":\"20260116233312.vld,20260115221045.vld\",\"Model\":\"O2Ring\",\"SN\":\"1234\"}";
    std::vector<String> files;
    bool ok = O2RingProtocol::parseInfoFileList((const uint8_t*)json, strlen(json), files);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_size_t(2, files.size());
    TEST_ASSERT_EQUAL_STRING("20260116233312.vld", files[0].c_str());
    TEST_ASSERT_EQUAL_STRING("20260115221045.vld", files[1].c_str());
}

void test_parse_info_filelist_empty() {
    const char* json = "{\"CurBAT\":\"100%\",\"FileList\":\"\",\"Model\":\"O2Ring\",\"SN\":\"1234\"}";
    std::vector<String> files;
    bool ok = O2RingProtocol::parseInfoFileList((const uint8_t*)json, strlen(json), files);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_size_t(0, files.size());
}

void test_parse_info_filelist_no_key() {
    const char* json = "{\"CurBAT\":\"100%\"}";
    std::vector<String> files;
    bool ok = O2RingProtocol::parseInfoFileList((const uint8_t*)json, strlen(json), files);
    TEST_ASSERT_FALSE(ok);
}

void test_parse_file_open_success() {
    // data[0]=0 (success), data[6..9] = 0x10, 0x8F, 0x00, 0x00 → 36624
    uint8_t data[12] = {0};
    data[0] = 0;
    data[6] = 0x10; data[7] = 0x8F; data[8] = 0x00; data[9] = 0x00;
    uint32_t fileSize = 0;
    bool ok = O2RingProtocol::parseFileOpenResponse(data, sizeof(data), fileSize);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT32(36624, fileSize);
}

void test_parse_file_open_error_status() {
    uint8_t data[12] = {0};
    data[0] = 9; // error
    uint32_t fileSize = 0;
    bool ok = O2RingProtocol::parseFileOpenResponse(data, sizeof(data), fileSize);
    TEST_ASSERT_FALSE(ok);
}

void test_parse_file_open_too_short() {
    uint8_t data[5] = {0};
    uint32_t fileSize = 0;
    bool ok = O2RingProtocol::parseFileOpenResponse(data, sizeof(data), fileSize);
    TEST_ASSERT_FALSE(ok);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_crc8_empty_input);
    RUN_TEST(test_crc8_single_byte);
    RUN_TEST(test_build_info_packet_structure);
    RUN_TEST(test_build_file_open_packet);
    RUN_TEST(test_build_file_read_block_encoding);
    RUN_TEST(test_expected_response_len);
    RUN_TEST(test_parse_info_filelist_two_files);
    RUN_TEST(test_parse_info_filelist_empty);
    RUN_TEST(test_parse_info_filelist_no_key);
    RUN_TEST(test_parse_file_open_success);
    RUN_TEST(test_parse_file_open_error_status);
    RUN_TEST(test_parse_file_open_too_short);
    return UNITY_END();
}
