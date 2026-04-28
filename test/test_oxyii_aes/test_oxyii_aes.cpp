// Native unit tests for OxyIIAes — AES-128/ECB/PKCS7 wrap/unwrap.

#include <unity.h>
#include <cstdint>
#include <cstring>

#include "OxyIIAes.h"
#include "../../src/OxyIIAes.cpp"

void setUp(void) {}
void tearDown(void) {}

// Published AES-128 ECB known-answer vector: key = zero, plaintext = zero.
// Block ciphertext (no padding) = 66e94bd4ef8a2c3b884cfa59ca342b2e.
// PKCS7-padded plaintext is 16 zero bytes + 16 bytes of 0x10, ciphertext
// is two blocks (32 bytes total).
void test_aes_known_vector_zero_key_zero_plaintext_first_block() {
    uint8_t key[16] = {0};
    uint8_t plaintext[16] = {0};
    uint8_t out[64] = {0};

    size_t n = OxyIIAes::encryptEcbPkcs7(plaintext, sizeof(plaintext), key,
                                          out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(32, n);  // PKCS7 always pads, even when block-aligned

    static const uint8_t kExpectedFirstBlock[16] = {
        0x66, 0xe9, 0x4b, 0xd4, 0xef, 0x8a, 0x2c, 0x3b,
        0x88, 0x4c, 0xfa, 0x59, 0xca, 0x34, 0x2b, 0x2e,
    };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kExpectedFirstBlock, out, 16);
}

void test_aes_roundtrip_short_message() {
    uint8_t key[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                       0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    const char* msg = "hello oxyii world";  // 17 bytes
    size_t msgLen = 17;

    uint8_t ciphertext[64];
    size_t ctLen = OxyIIAes::encryptEcbPkcs7(reinterpret_cast<const uint8_t*>(msg),
                                              msgLen, key, ciphertext, sizeof(ciphertext));
    TEST_ASSERT_EQUAL_size_t(32, ctLen);  // 17 → padded to 32 (next multiple of 16)

    // Ciphertext must NOT equal plaintext
    TEST_ASSERT_NOT_EQUAL_INT(0, memcmp(ciphertext, msg, msgLen));

    uint8_t plaintext[64] = {0};
    size_t ptLen = OxyIIAes::decryptEcbPkcs7(ciphertext, ctLen, key,
                                              plaintext, sizeof(plaintext));
    TEST_ASSERT_EQUAL_size_t(17, ptLen);
    TEST_ASSERT_EQUAL_INT(0, memcmp(plaintext, msg, msgLen));
}

void test_aes_roundtrip_block_aligned_input_pads_full_block() {
    uint8_t key[16] = {0};
    uint8_t plaintext[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    uint8_t ciphertext[64];

    size_t ctLen = OxyIIAes::encryptEcbPkcs7(plaintext, sizeof(plaintext), key,
                                              ciphertext, sizeof(ciphertext));
    TEST_ASSERT_EQUAL_size_t(32, ctLen);  // PKCS7 adds a full pad block

    uint8_t recovered[64] = {0};
    size_t ptLen = OxyIIAes::decryptEcbPkcs7(ciphertext, ctLen, key,
                                              recovered, sizeof(recovered));
    TEST_ASSERT_EQUAL_size_t(16, ptLen);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(plaintext, recovered, 16);
}

void test_aes_encrypt_returns_zero_when_outcap_too_small() {
    uint8_t key[16] = {0};
    uint8_t in[16] = {0};
    uint8_t out[20];  // need 32 for PKCS7-padded 16-byte input
    size_t n = OxyIIAes::encryptEcbPkcs7(in, sizeof(in), key, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(0, n);
}

void test_aes_decrypt_rejects_unaligned_ciphertext() {
    uint8_t key[16] = {0};
    uint8_t ct[15] = {0};
    uint8_t out[32] = {0};
    size_t n = OxyIIAes::decryptEcbPkcs7(ct, sizeof(ct), key, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(0, n);
}

void test_aes_decrypt_rejects_bad_padding() {
    // Build a "ciphertext" that decrypts to plaintext with invalid PKCS7 pad.
    // Easiest: encrypt a known plaintext, then modify the last byte of
    // ciphertext to force a bad-padding result. Padding validation rejects.
    uint8_t key[16] = {0};
    uint8_t in[16] = {0};
    uint8_t ct[64];
    size_t ctLen = OxyIIAes::encryptEcbPkcs7(in, sizeof(in), key, ct, sizeof(ct));
    TEST_ASSERT_EQUAL_size_t(32, ctLen);

    ct[ctLen - 1] ^= 0x55;  // corrupt one byte of the final ciphertext block

    uint8_t out[64] = {0};
    size_t n = OxyIIAes::decryptEcbPkcs7(ct, ctLen, key, out, sizeof(out));
    // The decrypted final block has random bytes in its tail, so PKCS7 unpad
    // should fail (or produce a valid-looking but wrong length). Either way,
    // n must NOT equal the original 16.
    TEST_ASSERT_NOT_EQUAL_size_t(16, n);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_aes_known_vector_zero_key_zero_plaintext_first_block);
    RUN_TEST(test_aes_roundtrip_short_message);
    RUN_TEST(test_aes_roundtrip_block_aligned_input_pads_full_block);
    RUN_TEST(test_aes_encrypt_returns_zero_when_outcap_too_small);
    RUN_TEST(test_aes_decrypt_rejects_unaligned_ciphertext);
    RUN_TEST(test_aes_decrypt_rejects_bad_padding);
    return UNITY_END();
}
