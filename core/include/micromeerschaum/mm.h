// SPDX-License-Identifier: Apache-2.0
#ifndef MICROMEERSCHAUM_MM_H
#define MICROMEERSCHAUM_MM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Wire-format version this core implements (spec/wire-format.md). */
#define MM_WIRE_VERSION 0u

/**
 * CRC-32/ISO-HDLC, chainable like zlib's crc32(): start with 0 and pass the
 * previous result to continue over the next chunk.
 */
uint32_t mm_crc32(uint32_t crc, const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
