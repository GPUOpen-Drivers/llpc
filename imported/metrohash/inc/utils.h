
/*******************************************************************************
 *
 * Trade secret of Advanced Micro Devices, Inc.
 * Copyright (c) 2018, Advanced Micro Devices, Inc., (unpublished)
 *
 * All rights reserved.
 * This notice is intended as a precaution against inadvertent publication and
 * does not imply publication or any waiver of confidentiality.
 * The year included in the foregoing notice is the year of creation of the work.
 *
 ******************************************************************************/

#ifndef METROHASH_UTIL_H
#define METROHASH_UTIL_H

#include <stdint.h>

namespace MetroHash
{

// 128-bit hash structure
struct Hash
{
    union
    {
        uint32_t dwords[4]; // Output hash in dwords.
        uint8_t  bytes[16]; // Output hash in bytes.
    };
};

// Compacts a 128-bit hash into a 64-bit one by XOR'ing the low and high 64-bits together.
//
// Takes input parameter pHash, which is 128-bit hash to be compacted.
//
// Returns 64-bit hash value based on the inputted 128-bit hash.
inline uint64_t Compact64(const Hash* pHash)
{
    return (static_cast<uint64_t>(pHash->dwords[3] ^ pHash->dwords[1]) |
           (static_cast<uint64_t>(pHash->dwords[2] ^ pHash->dwords[0]) << 32));
}

// Compacts a 64-bit hash checksum into a 32-bit one by XOR'ing each 32-bit chunk together.
//
// Takes input parameter pHash, which is 128-bit hash to be compacted.
//
// Returns 32-bit hash value based on the inputted 128-bit hash.
inline uint32_t Compact32(const Hash* pHash)
{
    return pHash->dwords[3] ^ pHash->dwords[2] ^ pHash->dwords[1] ^ pHash->dwords[0];
}

// Compacts a 64-bit hash checksum into a 32-bit one by XOR'ing each 32-bit chunk together.
//
// Takes input parameter hash, which is 64-bit hash to be compacted.
//
// Returns 32-bit hash value based on the inputted 64-bit hash.
inline uint32_t Compact32(uint64_t hash)
{
    return static_cast<uint32_t>(hash) ^ static_cast<uint32_t>(hash >> 32);
}

} // MetroHash

#endif // #ifndef METROHASH_UTIL_H
