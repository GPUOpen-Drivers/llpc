/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2023 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  vkgcMetroHash.h
 * @brief VKGC utility collection MetroHash namespace declarations.
 ***********************************************************************************************************************
 */
#pragma once

#include "metrohash.h"
#include <functional>
#include <utility>

// Namespace containing functions that provide support for MetroHash.
namespace MetroHash {

// 128-bit hash structure
struct Hash {
  union {
    uint64_t qwords[2]; // Output hash in qwords.
    uint32_t dwords[4]; // Output hash in dwords.
    uint8_t bytes[16];  // Output hash in bytes.
  };
};

// Compacts a 128-bit hash into a 64-bit one by XOR'ing the low and high 64-bits together.
//
// Takes input parameter hash, which is 128-bit hash to be compacted.
//
// Returns 64-bit hash value based on the inputted 128-bit hash.
inline uint64_t compact64(const Hash *hash) {
  return (static_cast<uint64_t>(hash->dwords[3] ^ hash->dwords[1]) |
          (static_cast<uint64_t>(hash->dwords[2] ^ hash->dwords[0]) << 32));
}

// Compacts a 64-bit hash checksum into a 32-bit one by XOR'ing each 32-bit chunk together.
//
// Takes input parameter hash, which is 128-bit hash to be compacted.
//
// Returns 32-bit hash value based on the inputted 128-bit hash.
inline unsigned compact32(const Hash *hash) {
  return hash->dwords[3] ^ hash->dwords[2] ^ hash->dwords[1] ^ hash->dwords[0];
}

// Compares two hashes. Returns true iff `lhs` is less than `rhs`.
inline bool operator<(const Hash &lhs, const Hash &rhs) {
  return std::make_pair(lhs.qwords[0], lhs.qwords[1]) < std::make_pair(rhs.qwords[0], rhs.qwords[1]);
}

// Compares two hashes for equality. Returns true iff both hashes are bit-identical.
inline bool operator==(const Hash &lhs, const Hash &rhs) {
  return lhs.qwords[0] == rhs.qwords[0] && lhs.qwords[1] == rhs.qwords[1];
}

// Compares two hashes for inequality. Returns true iff both hashes differ.
inline bool operator!=(const Hash &lhs, const Hash &rhs) {
  return !(lhs == rhs);
}

} // namespace MetroHash

// Make MetroHash::Hash compatible with std::hash, so that it can be used as a key type in unordered data structures.
namespace std {
template <> struct hash<MetroHash::Hash> {
  // Returns `hash` compacted to `size_t`. Returns zero for value-initialized hashes.
  size_t operator()(const MetroHash::Hash &hash) const { return static_cast<size_t>(MetroHash::compact64(&hash)); }
};
} // namespace std
