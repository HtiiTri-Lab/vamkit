// VaMSceneLoadNative.bin: reads one entry of a .var (zip) package into a caller buffer, for VaM Scene Load's fast morph
// delta reads (DAZMorph.LoadDeltasFromBinaryFile reads each .vmb through SharpZipLib's managed inflate, 4 bytes per call).
// It accepts exactly the entries SharpZipLib 0.86's ZipFile.GetInputStream would open without throwing (its
// TestLocalHeader(entry, Extract) checks) and returns an error for anything else, so the caller falls back to VaM's own
// read and gets VaM's own behaviour (and error) there. No state; safe from any thread. Inflate: miniz 10.2.0 tinfl (MIT).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "third_party/miniz/miniz.h"

extern "C" __declspec(dllexport) int SL_ApiVersion() { return 1; }

static bool ReadAt(HANDLE f, int64_t at, void* dst, DWORD n)
{
    OVERLAPPED o = {};
    o.Offset = (DWORD)at;
    o.OffsetHigh = (DWORD)(at >> 32);
    DWORD got = 0;
    return ReadFile(f, dst, n, &got, &o) && got == n;
}

// zipPath: the package file; headerOffset: offsetOfFirstEntry + the entry's local header offset (central directory);
// compSize / dstLen: the central directory's compressed / uncompressed sizes; method: the central directory's method,
// 0 (stored) or 8 (deflate); dst: dstLen bytes.
// Returns dstLen on success, or a negative code: -1 open, -2 read, -3 local header (signature, zip64 data, version,
// flags or sizes SharpZipLib would refuse, or anything this reader does not handle), -4 method, -5 inflate,
// -6 size, -7 memory.
extern "C" __declspec(dllexport) int64_t SL_ReadZipEntry(const wchar_t* zipPath, int64_t headerOffset, int64_t compSize,
                                                         int method, uint8_t* dst, int64_t dstLen)
{
    if (method != 0 && method != 8) return -4;
    if (compSize < 0 || dstLen < 0 || dstLen > 0x7fffffff || compSize > 0x7fffffff || (method == 0 && compSize != dstLen)) return -6;
    HANDLE f = CreateFileW(zipPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return -1;
    int64_t result = -2;
    uint8_t* src = nullptr;
    uint8_t* extra = nullptr;
    do {
        uint8_t h[30];
        if (!ReadAt(f, headerOffset, h, 30)) break;
        auto u16 = [&](int i) { return (uint32_t)(h[i] | (h[i + 1] << 8)); };
        auto u32 = [&](int i) { return (uint32_t)h[i] | ((uint32_t)h[i + 1] << 8) | ((uint32_t)h[i + 2] << 16) | ((uint32_t)h[i + 3] << 24); };
        if (u32(0) != 0x04034b50u) { result = -3; break; }
        int version = (int16_t)u16(4);
        uint32_t flags = u16(6);
        // the local sizes as SharpZipLib reads them: 32-bit fields, replaced by the zip64 extra field (tag 1: size,
        // then compressed size) when there is one
        int64_t size = u32(22), csize = u32(18);
        uint32_t nameLen = u16(26), extraLen = u16(28);
        bool zip64 = false;
        if (extraLen) {
            extra = (uint8_t*)malloc(extraLen);
            if (!extra) { result = -7; break; }
            if (!ReadAt(f, headerOffset + 30 + nameLen, extra, extraLen)) break;
            bool bad = false;
            for (uint32_t p = 0; p + 4 <= extraLen;) {
                uint32_t tag = extra[p] | (extra[p + 1] << 8), len = extra[p + 2] | (extra[p + 3] << 8);
                if (tag == 1) {
                    if (len < 16 || p + 4 + 16 > extraLen) { bad = true; break; }   // SharpZipLib's ReadLong would throw
                    memcpy(&size, extra + p + 4, 8);
                    memcpy(&csize, extra + p + 12, 8);
                    zip64 = true;
                    break;
                }
                p += 4 + len;
            }
            if (bad) { result = -3; break; }
        }
        if (zip64) {
            if (flags & 8u) {
                if ((size != -1 && size != dstLen) || (csize != -1 && csize != compSize)) { result = -3; break; }
            }
        } else if (version >= 45 && ((uint32_t)size == 0xffffffffu || (uint32_t)csize == 0xffffffffu)) { result = -3; break; }
        // TestLocalHeader's Extract checks: supported version, no patched data / strong encryption / masked header,
        // and no traditional encryption (the caller also refuses encrypted entries)
        if (version > 51 || (version > 20 && version < 45)) { result = -3; break; }
        if ((flags & 0x3060u) != 0 || (flags & 1u) != 0) { result = -3; break; }
        // sizes against the central directory, unless a data descriptor leaves them 0
        if ((flags & 8u) == 0 || size > 0 || csize > 0) {
            if (size != dstLen || (csize != compSize && csize != -1)) { result = -3; break; }
        }
        int64_t data = headerOffset + 30 + nameLen + extraLen;
        uint8_t* target = method == 0 ? dst : nullptr;
        if (!target) {
            src = (uint8_t*)malloc(compSize ? (size_t)compSize : 1);
            if (!src) { result = -7; break; }
            target = src;
        }
        if (compSize && !ReadAt(f, data, target, (DWORD)compSize)) break;
        if (method == 0) { result = dstLen; break; }
        size_t out = tinfl_decompress_mem_to_mem(dst, (size_t)dstLen, src, (size_t)compSize,
                                                 TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        if (out == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) { result = -5; break; }
        result = (int64_t)out == dstLen ? dstLen : -6;
    } while (false);
    free(src);
    free(extra);
    CloseHandle(f);
    return result;
}
