#include "MapBakerPng.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <windows.h>
#include <string>
#include <zlib.h>

namespace {

    // Big-endian 32-bit writer.
    void PutU32(std::vector<uint8_t>& out, uint32_t value) {
        out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(value & 0xFF));
    }

    void PutU16(std::vector<uint8_t>& out, uint16_t value) {
        out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(value & 0xFF));
    }

    // Append a PNG chunk (type + data), computing the CRC over both.
    void AppendChunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data, uint32_t length) {
        PutU32(out, length);
        const size_t offset = out.size();
        out.insert(out.end(), type, type + 4);
        if (data && length) {
            out.insert(out.end(), data, data + length);
        }

        uLong crc = crc32(0L, out.data() + offset, static_cast<uInt>(4 + length));
        PutU32(out, static_cast<uint32_t>(crc));
    }

    bool WideFromUtf8(const std::string& utf8, std::wstring& wide) {
        int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
        if (length <= 0) {
            return false;
        }

        wide.resize(static_cast<size_t>(length));
        return MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &wide[0], length) == length;
    }

}

namespace MapBakerPng {
    bool Write(const char* path, int32_t width, int32_t height, const uint8_t* rgb) {
        if (width <= 0 || height <= 0 || !rgb) {
            return false;
        }

        uint32_t stride = static_cast<uint32_t>(width) * 3;

        // Build the filtered scanlines: each row is preceded by a filter byte of 0 (None).
        std::vector<uint8_t> raw;
        raw.reserve(static_cast<size_t>(height) * (1 + stride));
        for (int32_t y = 0; y < height; y++) {
            raw.push_back(0x00);
            const uint8_t* row = rgb + static_cast<size_t>(y) * stride;
            raw.insert(raw.end(), row, row + stride);
        }

        uLongf bound = compressBound(static_cast<uLong>(raw.size()));
        std::vector<uint8_t> idat(bound);
        int result = compress2(idat.data(), &bound, raw.data(), static_cast<uLong>(raw.size()), 6);
        if (result != Z_OK) {
            return false;
        }

        idat.resize(bound);

        std::vector<uint8_t> png;
        // Signature.
        static const uint8_t signature[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
        png.insert(png.end(), signature, signature + 8);

        // IHDR: width, height, bit depth 8, color type 2 (RGB), compression 0, filter 0, interlace 0.
        std::vector<uint8_t> ihdr;
        PutU32(ihdr, static_cast<uint32_t>(width));
        PutU32(ihdr, static_cast<uint32_t>(height));
        ihdr.push_back(8);
        ihdr.push_back(2);
        ihdr.push_back(0);
        ihdr.push_back(0);
        ihdr.push_back(0);
        AppendChunk(png, "IHDR", ihdr.data(), static_cast<uint32_t>(ihdr.size()));

        AppendChunk(png, "IDAT", idat.data(), static_cast<uint32_t>(idat.size()));
        AppendChunk(png, "IEND", nullptr, 0);

        // Atomic write: .tmp then move into place.
        std::string tmpPath(std::string(path) + ".tmp");
        std::wstring tmpWide;
        std::wstring destWide;

        if (!WideFromUtf8(tmpPath, tmpWide) || !WideFromUtf8(path, destWide)) {
            return false;
        }

        FILE* file = _wfopen(tmpWide.c_str(), L"wb");

        if (!file) {
            return false;
        }

        bool ok = fwrite(png.data(), 1, png.size(), file) == png.size();
        fclose(file);

        if (!ok) {
            DeleteFileW(tmpWide.c_str());
            return false;
        }

        if (!MoveFileExW(tmpWide.c_str(), destWide.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(tmpWide.c_str());
            return false;
        }

        return true;
    }
}
