#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// =============================================================================
//  Reads NCW sample files (the compressed WAV format some sample libraries
//  ship their sounds in), so a library you own can be played in the
//  Multi Sampler. Nothing is copied or converted on disk.
//
//  Layout: a 120-byte header, a table of block offsets, then blocks of 512
//  sample frames. Each block holds one part per channel:
//    16-byte part header: id, base value, bit width, flags
//    bit width > 0   512 deltas of that many bits (each sample = previous + delta)
//    bit width < 0   512 samples of that many bits, stored directly
//    bit width = 0   512 samples at the file's full bit depth
//    flags = 1       the two channels are mid / side
// =============================================================================

namespace rs::ncw
{

struct Audio
{
    int channels = 0, bitsPerSample = 0, sampleRate = 0;
    std::vector<std::vector<float>> data;   // [channel][frame], -1..1
    std::string error;
};

namespace detail
{
    inline uint32_t u32 (const uint8_t* p) { return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24); }
    inline uint16_t u16 (const uint8_t* p) { return (uint16_t) (p[0] | (p[1] << 8)); }

    // n values of `bits` bits each, least significant bit first, as signed (two's complement) or unsigned
    inline void unpack (const uint8_t* src, size_t avail, int bits, bool isSigned, int64_t* out, int n)
    {
        uint64_t acc = 0;
        int have = 0;
        size_t pos = 0;
        for (int i = 0; i < n; ++i)
        {
            while (have < bits)
            {
                const uint64_t byte = pos < avail ? src[pos] : 0;
                acc |= byte << have;
                ++pos;
                have += 8;
            }
            uint64_t v = bits >= 64 ? acc : (acc & ((1ull << bits) - 1));
            acc = bits >= 64 ? 0 : (acc >> bits);
            have -= bits;
            int64_t s = (int64_t) v;
            if (isSigned && bits < 64 && (v & (1ull << (bits - 1)))) s = (int64_t) v - (int64_t) (1ull << bits);
            out[i] = s;
        }
    }
}

inline bool isNcw (const uint8_t* d, size_t size)
{
    return size >= 120 && d[0] == 0x01 && d[1] == 0xa8 && d[2] == 0x9e && d[3] == 0xd6;
}

inline Audio decode (const uint8_t* d, size_t size)
{
    using namespace detail;
    Audio a;
    if (! isNcw (d, size)) { a.error = "not an NCW file"; return a; }
    a.channels = u16 (d + 8);
    a.bitsPerSample = u16 (d + 10);
    a.sampleRate = (int) u32 (d + 12);
    const uint32_t frames = u32 (d + 16), tableAt = u32 (d + 20), blocksAt = u32 (d + 24), blocksLen = u32 (d + 28);
    if (a.channels < 1 || a.channels > 8 || a.bitsPerSample < 8 || a.bitsPerSample > 32 || a.sampleRate < 1000
        || tableAt < 120 || blocksAt < tableAt || (size_t) blocksAt > size || frames == 0)
    { a.error = "unexpected NCW header"; return a; }
    (void) blocksLen;

    const size_t numBlocks = (blocksAt - tableAt) / 4;
    a.data.assign ((size_t) a.channels, std::vector<float> (frames, 0.0f));
    const double scale = 1.0 / (double) (1ll << (a.bitsPerSample - 1));
    std::vector<int64_t> vals (512), chan[8];
    for (auto& c : chan) c.resize (512);

    size_t frame = 0;
    for (size_t b = 0; b < numBlocks && frame < frames; ++b)
    {
        size_t at = (size_t) blocksAt + u32 (d + tableAt + b * 4);
        bool midSide = false;
        for (int c = 0; c < a.channels; ++c)
        {
            if (at + 16 > size || u32 (d + at) != 0x3e9a0c16) { a.error = "damaged NCW block"; return a; }
            const int32_t base = (int32_t) u32 (d + at + 4);
            const int16_t bits = (int16_t) u16 (d + at + 8);
            const uint16_t flags = u16 (d + at + 10);
            midSide = midSide || (flags & 1) != 0;
            at += 16;
            const int width = bits == 0 ? a.bitsPerSample : (bits < 0 ? -bits : bits);
            const size_t bytes = (size_t) width * 512 / 8;
            if (at + bytes > size) { a.error = "truncated NCW block"; return a; }
            auto& out = chan[c];
            if (bits > 0)
            {
                unpack (d + at, bytes, width, true, vals.data(), 512);
                int64_t prev = base;
                for (int i = 0; i < 512; ++i) { out[(size_t) i] = prev; prev += vals[(size_t) i]; }
            }
            else
            {
                unpack (d + at, bytes, width, true, out.data(), 512);
            }
            at += bytes;
        }
        const size_t n = std::min<size_t> (512, frames - frame);
        for (size_t i = 0; i < n; ++i)
        {
            if (midSide && a.channels == 2)
            {
                const int64_t m = chan[0][i], s = chan[1][i];
                a.data[0][frame + i] = (float) ((double) (m + s) * scale);
                a.data[1][frame + i] = (float) ((double) (m - s) * scale);
            }
            else
                for (int c = 0; c < a.channels; ++c) a.data[(size_t) c][frame + i] = (float) ((double) chan[c][i] * scale);
        }
        frame += n;
    }
    return a;
}

} // namespace rs::ncw
