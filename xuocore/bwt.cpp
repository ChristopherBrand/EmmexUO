// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Port of ClassicUO's BwtDecompress (src/ClassicUO.Utility/BwtDecompress.cs),
// Copyright (c) ClassicUO contributors, BSD-2-Clause. Kept structurally identical to the
// original so the two can be compared line by line.

#include "bwt.h"

#include <string.h>

static void bwt_build_table(std::vector<uint16_t> &table, uint8_t /*startValue*/)
{
    // The original enumerates (startValue + i) as a (low, high) byte pair for every i and then
    // sorts. That visits each of the 65536 pairs exactly once, so the sorted table is always
    // the identity, whatever startValue is. Filling it directly skips a 64K sort per entry.
    for (int i = 0; i < 256 * 256; i++)
    {
        table[i] = uint16_t(i);
    }
}

static void bwt_shift_left(uint8_t *input, int max)
{
    for (int i = 0; i < max; ++i)
    {
        input[i] = input[i + 1];
    }
}

static void bwt_frequency(const int32_t *input, uint8_t *output)
{
    int32_t tmp[256];
    memcpy(tmp, input, sizeof(tmp));

    for (int i = 0; i < 256; i++)
    {
        uint32_t value = 0;
        uint8_t index = 0;
        for (int j = 0; j < 256; j++)
        {
            if (uint32_t(tmp[j]) > value && tmp[j] > 0)
            {
                index = uint8_t(j);
                value = uint32_t(tmp[j]);
            }
        }
        if (value == 0)
        {
            break;
        }
        output[i] = index;
        tmp[index] = 0;
    }
}

static bool bwt_internal_decompress(const uint8_t *input, size_t inputLen, std::vector<uint8_t> &out)
{
    // 256 symbol counts (as little-endian int32) precede the payload.
    if (inputLen < 1024)
    {
        return false;
    }

    uint8_t symbolTable[256];
    uint8_t frequency[256] = {};
    int32_t partialInput[256 * 3] = {};

    for (int i = 0; i < 256; i++)
    {
        symbolTable[i] = uint8_t(i);
    }
    memcpy(partialInput, input, 1024);

    int64_t sum = 0;
    int nonZeroCount = 0;
    for (int i = 0; i < 256; i++)
    {
        if (partialInput[i] < 0)
        {
            return false;
        }
        sum += partialInput[i];
        if (partialInput[i] != 0)
        {
            nonZeroCount++;
        }
    }
    if (sum <= 0 || size_t(sum) > inputLen * 64)
    {
        return false; // implausible output size for this input
    }
    const size_t len = size_t(sum);

    bwt_frequency(partialInput, frequency);

    for (int i = 0, m = 0; i < nonZeroCount; ++i)
    {
        const uint8_t freq = frequency[i];
        if (size_t(m) + 1024 >= inputLen)
        {
            return false;
        }
        symbolTable[input[m + 1024]] = freq;
        partialInput[freq + 256] = m + 1;
        m += partialInput[freq];
        partialInput[freq + 512] = m;
    }

    out.assign(len, 0);
    uint8_t val = symbolTable[0];
    size_t count = 0;
    do
    {
        int32_t &firstValRef = partialInput[val + 256];
        out[count] = val;

        if (firstValRef >= partialInput[val + 512])
        {
            if (nonZeroCount-- > 0)
            {
                bwt_shift_left(symbolTable, nonZeroCount);
                val = symbolTable[0];
            }
        }
        else
        {
            if (size_t(firstValRef) + 1024 >= inputLen)
            {
                out.clear();
                return false;
            }
            const uint8_t idx = input[firstValRef + 1024];
            firstValRef++;

            if (idx != 0)
            {
                bwt_shift_left(symbolTable, idx);
                symbolTable[idx] = val;
                val = symbolTable[0];
            }
        }
        count++;
    } while (count < len);

    return true;
}

bool uo_bwt_decompress(const uint8_t *src, size_t len, std::vector<uint8_t> &out)
{
    out.clear();
    // u32 header, then one byte per symbol
    if (len < 5)
    {
        return false;
    }

    std::vector<uint16_t> table(256 * 256);
    size_t pos = 4;
    uint8_t firstChar = src[pos++];
    bwt_build_table(table, firstChar);

    // Move-to-front decode. Like the original, the final byte read is not decoded, and the
    // list keeps its last slot zeroed.
    std::vector<uint8_t> list(len - 4, 0);
    size_t i = 0;
    while (pos < len)
    {
        uint8_t currentValue = firstChar;
        const uint16_t value = table[currentValue];
        if (currentValue > 0)
        {
            do
            {
                table[currentValue] = table[currentValue - 1];
            } while (--currentValue > 0);
        }
        table[0] = value;
        list[i++] = uint8_t(value);
        firstChar = src[pos++];
    }

    return bwt_internal_decompress(list.data(), list.size(), out);
}
