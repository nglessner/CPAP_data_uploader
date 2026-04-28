// Unity tests for OxyIIProtocol — port of admin/sleep's
// scripts/o2r_pair/test_oxyii_protocol.py. Exercises CRC, frame encode/decode,
// session-key derivation, and reply parsers against the same captured ViHealth
// snoop frames used in the Python suite.

#include <unity.h>
#include <cstdint>
#include <cstring>

#include "OxyIIProtocol.h"

using namespace OxyIIProtocol;

void setUp(void) {}
void tearDown(void) {}

static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode a hex string like "a5e11e00020000bf" into out[]. Returns byte count.
static size_t fromHex(const char* hex, uint8_t* out, size_t outCap) {
    size_t n = 0;
    while (hex[0] && hex[1] && n < outCap) {
        int hi = hexNibble(hex[0]);
        int lo = hexNibble(hex[1]);
        if (hi < 0 || lo < 0) break;
        out[n++] = static_cast<uint8_t>((hi << 4) | lo);
        hex += 2;
    }
    return n;
}

// ----- CRC: snoop-frame regression (matches Python SNOOP_FRAMES) -----

void test_crc_get_info_seq2_matches_snoop() {
    // a5 e1 1e 00 02 00 00 bf
    uint8_t frame[16];
    size_t n = fromHex("a5e11e00020000bf", frame, sizeof(frame));
    TEST_ASSERT_EQUAL_size_t(8, n);
    TEST_ASSERT_EQUAL_UINT8(0xBF, crc8(frame, n - 1));
}

void test_crc_set_utc_time_seq1_matches_snoop() {
    uint8_t frame[32];
    size_t n = fromHex("a5c03f00010800ea07041b0b0808ce8a", frame, sizeof(frame));
    TEST_ASSERT_EQUAL_UINT8(0x8A, crc8(frame, n - 1));
}

void test_crc_keepalive_matches_snoop() {
    uint8_t frame[16];
    size_t n = fromHex("a510ef000001000007", frame, sizeof(frame));
    TEST_ASSERT_EQUAL_UINT8(0x07, crc8(frame, n - 1));
}

void test_crc_handshake_matches_snoop() {
    uint8_t frame[16];
    size_t n = fromHex("a500ff00010000d3", frame, sizeof(frame));
    TEST_ASSERT_EQUAL_UINT8(0xD3, crc8(frame, n - 1));
}

void test_crc_auth_with_encrypted_payload_matches_snoop() {
    // cmd=0xFF, encrypted setup payload — captures show 16-byte payload + crc
    uint8_t frame[64];
    size_t n = fromHex(
        "a5ff00000010000068158872091cb098c8c7da23b04ccfb7", frame, sizeof(frame));
    TEST_ASSERT_EQUAL_UINT8(0xB7, crc8(frame, n - 1));
}

// ----- encodeFrame: must produce exact ViHealth bytes -----

void test_encode_get_info_matches_snoop() {
    uint8_t out[16];
    size_t n = encodeFrame(OP_GET_INFO, nullptr, 0, /*seq=*/2, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(8, n);
    TEST_ASSERT_EQUAL_UINT8(0xA5, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0xE1, out[1]);
    TEST_ASSERT_EQUAL_UINT8(0x1E, out[2]);
    TEST_ASSERT_EQUAL_UINT8(0x00, out[3]);
    TEST_ASSERT_EQUAL_UINT8(0x02, out[4]);
    TEST_ASSERT_EQUAL_UINT8(0x00, out[5]);
    TEST_ASSERT_EQUAL_UINT8(0x00, out[6]);
    TEST_ASSERT_EQUAL_UINT8(0xBF, out[7]);
}

void test_encode_set_utc_time_matches_snoop() {
    uint8_t payload[8];
    size_t pn = fromHex("ea07041b0b0808ce", payload, sizeof(payload));
    TEST_ASSERT_EQUAL_size_t(8, pn);

    uint8_t out[32];
    size_t n = encodeFrame(0xC0, payload, pn, /*seq=*/1, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(16, n);

    uint8_t expected[16];
    fromHex("a5c03f00010800ea07041b0b0808ce8a", expected, sizeof(expected));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, 16);
}

void test_encode_large_payload_uses_little_endian_length() {
    // 768-byte payload — verifies len_lo/len_hi split
    uint8_t payload[768];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = static_cast<uint8_t>(i & 0xFF);

    uint8_t out[800];
    size_t n = encodeFrame(0xF3, payload, sizeof(payload), /*seq=*/7, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(7 + 768 + 1, n);
    TEST_ASSERT_EQUAL_UINT8(0x07, out[4]);  // seq
    TEST_ASSERT_EQUAL_UINT8(0x00, out[5]);  // len_lo
    TEST_ASSERT_EQUAL_UINT8(0x03, out[6]);  // len_hi
}

void test_encode_returns_zero_when_outcap_too_small() {
    uint8_t out[4];
    size_t n = encodeFrame(OP_GET_INFO, nullptr, 0, 0, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(0, n);
}

// ----- decodeFrame -----

void test_decode_roundtrip_with_payload() {
    uint8_t raw[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    uint8_t encoded[32];
    size_t n = encodeFrame(0xF3, raw, sizeof(raw), /*seq=*/42, encoded, sizeof(encoded));

    DecodedFrame d = decodeFrame(encoded, n);
    TEST_ASSERT_TRUE(d.ok);
    TEST_ASSERT_EQUAL_UINT8(0xF3, d.opcode);
    TEST_ASSERT_EQUAL_UINT8(42, d.seq);
    TEST_ASSERT_EQUAL_size_t(7, d.payloadOffset);
    TEST_ASSERT_EQUAL_size_t(10, d.payloadLen);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(raw, encoded + d.payloadOffset, d.payloadLen);
}

void test_decode_rejects_bad_lead() {
    uint8_t encoded[16];
    size_t n = encodeFrame(OP_GET_INFO, nullptr, 0, 0, encoded, sizeof(encoded));
    encoded[0] = 0xAA;

    DecodedFrame d = decodeFrame(encoded, n);
    TEST_ASSERT_FALSE(d.ok);
    TEST_ASSERT_EQUAL_UINT8(DECODE_BAD_LEAD, d.errorCode);
}

void test_decode_rejects_bad_complement() {
    uint8_t encoded[16];
    size_t n = encodeFrame(OP_GET_INFO, nullptr, 0, 0, encoded, sizeof(encoded));
    encoded[2] = 0x00;

    DecodedFrame d = decodeFrame(encoded, n);
    TEST_ASSERT_FALSE(d.ok);
    TEST_ASSERT_EQUAL_UINT8(DECODE_BAD_COMPLEMENT, d.errorCode);
}

void test_decode_rejects_bad_crc() {
    uint8_t encoded[16];
    size_t n = encodeFrame(OP_GET_INFO, nullptr, 0, 0, encoded, sizeof(encoded));
    encoded[n - 1] ^= 0x55;

    DecodedFrame d = decodeFrame(encoded, n);
    TEST_ASSERT_FALSE(d.ok);
    TEST_ASSERT_EQUAL_UINT8(DECODE_BAD_CRC, d.errorCode);
}

void test_decode_rejects_short_frame() {
    uint8_t tiny[3] = {0xA5, 0xE1, 0x1E};
    DecodedFrame d = decodeFrame(tiny, sizeof(tiny));
    TEST_ASSERT_FALSE(d.ok);
    TEST_ASSERT_EQUAL_UINT8(DECODE_TOO_SHORT, d.errorCode);
}

void test_decode_rejects_length_mismatch() {
    uint8_t encoded[16];
    size_t n = encodeFrame(OP_GET_INFO, nullptr, 0, 0, encoded, sizeof(encoded));
    // claim the frame holds 5 payload bytes when really 0
    encoded[5] = 5;

    DecodedFrame d = decodeFrame(encoded, n);
    TEST_ASSERT_FALSE(d.ok);
    TEST_ASSERT_EQUAL_UINT8(DECODE_BAD_LENGTH, d.errorCode);
}

// ----- Session key derivation -----

void test_derive_session_key_layout() {
    uint8_t key[16];
    bool ok = deriveSessionKey("25B2303210", 10, /*ts=*/0x12345678ULL, key);
    TEST_ASSERT_TRUE(ok);
    // [0..7] = even bytes of MD5("lepucloud")
    static const uint8_t kSalt[8] = {0xc2, 0xcf, 0xda, 0xd8, 0xa8, 0xf7, 0xc4, 0x35};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kSalt, key, 8);
    // [8..11] = "25B2"
    TEST_ASSERT_EQUAL_UINT8('2', key[8]);
    TEST_ASSERT_EQUAL_UINT8('5', key[9]);
    TEST_ASSERT_EQUAL_UINT8('B', key[10]);
    TEST_ASSERT_EQUAL_UINT8('2', key[11]);
    // [12..15] = ts >> 0,1,2,3 (NOT byte-extract — quirky AAR shift)
    for (int n = 0; n < 4; n++) {
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>((0x12345678ULL >> n) & 0xFF), key[12 + n]);
    }
}

void test_derive_session_key_short_serial_fails() {
    uint8_t key[16];
    bool ok = deriveSessionKey("AB", 2, 0, key);
    TEST_ASSERT_FALSE(ok);
}

// ----- parseGetInfoReply: validated against captured ViHealth snoop -----

// Captured 2026-04-27 from a real ViHealth-app GET_INFO response on this user's
// T8520. Full notify frame; payload is bytes [7..-1] (60 bytes).
static const char kSnoopGetInfoFrame[] =
    "a5e11e01023c00420005000102000000324430313030303201400852160100"
    "ea07041b0b0808e007000000000a3235423233303332313000000000000000"
    "000000000016";

void test_parse_get_info_extracts_serial_and_firmware() {
    uint8_t frame[128];
    size_t fn = fromHex(kSnoopGetInfoFrame, frame, sizeof(frame));
    DecodedFrame d = decodeFrame(frame, fn);
    TEST_ASSERT_TRUE(d.ok);
    TEST_ASSERT_EQUAL_UINT8(OP_GET_INFO, d.opcode);
    TEST_ASSERT_EQUAL_size_t(60, d.payloadLen);

    DeviceInfo info = parseGetInfoReply(frame + d.payloadOffset, d.payloadLen);
    TEST_ASSERT_TRUE(info.ok);
    TEST_ASSERT_EQUAL_STRING("2D010002", info.firmwareVersion);
    TEST_ASSERT_EQUAL_STRING("25B2303210", info.serialNumber);
}

void test_parse_get_info_too_short_returns_not_ok() {
    uint8_t tiny[10] = {0};
    DeviceInfo info = parseGetInfoReply(tiny, sizeof(tiny));
    TEST_ASSERT_FALSE(info.ok);
}

// ----- READ_FILE_START / DATA payload builders -----

void test_build_read_file_start_payload_filename_and_type() {
    uint8_t out[20];
    const char* fn = "20260427213521";  // 14 chars, typical T8520 filename
    bool ok = buildReadFileStartPayload(fn, strlen(fn), 0, out, sizeof(out));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING_LEN(fn, reinterpret_cast<const char*>(out), 14);
    // Bytes 14..15 must be null-pad
    TEST_ASSERT_EQUAL_UINT8(0, out[14]);
    TEST_ASSERT_EQUAL_UINT8(0, out[15]);
    // Bytes 16..19 = file_type u32 LE
    TEST_ASSERT_EQUAL_UINT8(0, out[16]);
    TEST_ASSERT_EQUAL_UINT8(0, out[17]);
    TEST_ASSERT_EQUAL_UINT8(0, out[18]);
    TEST_ASSERT_EQUAL_UINT8(0, out[19]);
}

void test_build_read_file_data_payload_offset_le() {
    uint8_t out[8];
    bool ok = buildReadFileDataPayload(0x12345678, out, sizeof(out));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(0x78, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0x56, out[1]);
    TEST_ASSERT_EQUAL_UINT8(0x34, out[2]);
    TEST_ASSERT_EQUAL_UINT8(0x12, out[3]);
}

void test_build_read_file_data_payload_outcap_too_small() {
    uint8_t out[3];
    bool ok = buildReadFileDataPayload(0, out, sizeof(out));
    TEST_ASSERT_FALSE(ok);
}

// ----- File list iteration -----

void test_file_list_parses_two_entries() {
    // count=2, two 16-byte slots: "20260427105949" + 2 nulls, "20260427110015" + 2 nulls
    uint8_t buf[33];
    buf[0] = 2;
    const char* a = "20260427105949";
    const char* b = "20260427110015";
    memset(buf + 1, 0, 32);
    memcpy(buf + 1, a, strlen(a));
    memcpy(buf + 17, b, strlen(b));

    FileListIter it = beginFileList(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT8(2, it.count);

    char name[MAX_FILE_NAME + 2];
    TEST_ASSERT_TRUE(nextFilename(it, name));
    TEST_ASSERT_EQUAL_STRING("20260427105949", name);
    TEST_ASSERT_TRUE(nextFilename(it, name));
    TEST_ASSERT_EQUAL_STRING("20260427110015", name);
    TEST_ASSERT_FALSE(nextFilename(it, name));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_crc_get_info_seq2_matches_snoop);
    RUN_TEST(test_crc_set_utc_time_seq1_matches_snoop);
    RUN_TEST(test_crc_keepalive_matches_snoop);
    RUN_TEST(test_crc_handshake_matches_snoop);
    RUN_TEST(test_crc_auth_with_encrypted_payload_matches_snoop);
    RUN_TEST(test_encode_get_info_matches_snoop);
    RUN_TEST(test_encode_set_utc_time_matches_snoop);
    RUN_TEST(test_encode_large_payload_uses_little_endian_length);
    RUN_TEST(test_encode_returns_zero_when_outcap_too_small);
    RUN_TEST(test_decode_roundtrip_with_payload);
    RUN_TEST(test_decode_rejects_bad_lead);
    RUN_TEST(test_decode_rejects_bad_complement);
    RUN_TEST(test_decode_rejects_bad_crc);
    RUN_TEST(test_decode_rejects_short_frame);
    RUN_TEST(test_decode_rejects_length_mismatch);
    RUN_TEST(test_derive_session_key_layout);
    RUN_TEST(test_derive_session_key_short_serial_fails);
    RUN_TEST(test_parse_get_info_extracts_serial_and_firmware);
    RUN_TEST(test_parse_get_info_too_short_returns_not_ok);
    RUN_TEST(test_build_read_file_start_payload_filename_and_type);
    RUN_TEST(test_build_read_file_data_payload_offset_le);
    RUN_TEST(test_build_read_file_data_payload_outcap_too_small);
    RUN_TEST(test_file_list_parses_two_entries);
    return UNITY_END();
}
