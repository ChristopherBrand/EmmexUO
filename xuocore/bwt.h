// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vector>

// Decodes the second stage of UOP compression type 3 ("zlib + BWT"), used by newer
// gumpartLegacyMUL.uop files. The input is the already zlib-inflated block.
// Returns false (and leaves out empty) if the data is malformed.
bool uo_bwt_decompress(const uint8_t *src, size_t len, std::vector<uint8_t> &out);
