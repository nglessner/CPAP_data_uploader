#ifndef OXYII_AES_H
#define OXYII_AES_H

// AES-128/ECB/PKCS7 wrap and unwrap for the OxyII protocol.
//
// Two opcodes use encryption: 0xFF (AUTH) and 0xF2 (READ_FILE_START). All
// replies in the pull path are plaintext, so only the outgoing direction is
// strictly required — but decrypt is provided too, both for the roundtrip
// regression test and for future opcodes.
//
// On ESP32 (Arduino/ESP-IDF), this is implemented via mbedtls (already linked
// for HTTPS in SleepHQUploader). On the native test env, a self-contained
// AES-128 ECB reference implementation lives in OxyIIAes.cpp under #ifdef
// UNIT_TEST. Public API is identical on both platforms.

#include <cstdint>
#include <cstddef>

namespace OxyIIAes {

constexpr size_t BLOCK_SIZE = 16;
constexpr size_t KEY_SIZE   = 16;

// Encrypt `plaintextLen` bytes with AES-128/ECB/PKCS7. The output is at least
// `((plaintextLen / 16) + 1) * 16` bytes — PKCS7 always adds a full block when
// the input is already block-aligned.
//
// Returns the number of ciphertext bytes written, or 0 on failure (outCap too
// small, key bad).
size_t encryptEcbPkcs7(const uint8_t* plaintext, size_t plaintextLen,
                       const uint8_t key[KEY_SIZE],
                       uint8_t* out, size_t outCap);

// Decrypt `ciphertextLen` bytes (must be a multiple of 16) and strip the
// trailing PKCS7 padding.
//
// Returns the number of plaintext bytes written, or 0 on failure (alignment,
// outCap too small, padding invalid).
size_t decryptEcbPkcs7(const uint8_t* ciphertext, size_t ciphertextLen,
                       const uint8_t key[KEY_SIZE],
                       uint8_t* out, size_t outCap);

}  // namespace OxyIIAes

#endif  // OXYII_AES_H
