#include "Crc.h"
#include <fstream>
#include <zlib.h>

namespace Crc {

// ─── CRC8 ─────────────────────────────────────────────────────────────────────

uint8_t crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0x00;
    while (len--) {
        crc ^= *data++;
        for (int i = 0; i < 8; ++i) {
            if (crc & 0x80)
                crc = static_cast<uint8_t>((crc << 1) ^ 0x07);
            else
                crc <<= 1;
        }
    }
    return crc;
}

// ─── CRC32 (zlib) ─────────────────────────────────────────────────────────────

uint32_t crc32(const uint8_t* data, size_t len) {
    return static_cast<uint32_t>(::crc32(0, data, static_cast<uInt>(len)));
}

uint32_t crc32_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;

    uLong crc = ::crc32(0, nullptr, 0);
    char buf[4096];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
        crc = ::crc32(crc, reinterpret_cast<const Bytef*>(buf),
                      static_cast<uInt>(f.gcount()));
    }
    return static_cast<uint32_t>(crc);
}

} // namespace Crc
