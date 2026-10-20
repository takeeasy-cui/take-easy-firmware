/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * mbedTLS 协议摘要适配。/ mbedTLS protocol-digest adapter.
 * 冻结：MD5 仅兼容网页协议，不用于 TLS。/ Frozen: MD5 for protocol compatibility only, never TLS.
 */
#pragma once
#include "Arduino.h"
#include "psa/crypto.h"
#include <cstdio>
class MD5Builder {
    psa_hash_operation_t ctx_ = PSA_HASH_OPERATION_INIT;
    uint8_t digest_[16] = {};
    bool ok_ = true;
public:
    ~MD5Builder() { psa_hash_abort(&ctx_); }
    void begin() { psa_hash_abort(&ctx_); ok_ = psa_crypto_init() == PSA_SUCCESS && psa_hash_setup(&ctx_, PSA_ALG_MD5) == PSA_SUCCESS; }
    void add(const uint8_t* bytes, size_t size) { ok_ = ok_ && psa_hash_update(&ctx_, bytes, size) == PSA_SUCCESS; }
    void calculate() { size_t length; ok_ = ok_ && psa_hash_finish(&ctx_, digest_, sizeof(digest_), &length) == PSA_SUCCESS && length == sizeof(digest_); }
    String toString() const {
        if (!ok_) return {};
        char hex[33];
        for (int i = 0; i < 16; ++i) snprintf(hex + i * 2, 3, "%02x", digest_[i]);
        return hex;
    }
};
