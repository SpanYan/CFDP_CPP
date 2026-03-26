#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
//  CRC8  — utilisé pour valider chaque paquet DataPdu
//  CRC32 — utilisé pour valider le fichier complet
//
//  Implémentation software embarquée, sans dépendance externe.
//  Pour la production : remplacer CRC32 par zlib::crc32() si disponible.
// ─────────────────────────────────────────────────────────────────────────────

namespace Crc {

// CRC8 polynomial 0x07 (Bluetooth/CCITT)
uint8_t  crc8 (const uint8_t* data, size_t len);

// CRC32 polynomial 0xEDB88320 (IEEE 802.3)
uint32_t crc32(const uint8_t* data, size_t len);
uint32_t crc32_file(const std::string& path);

} // namespace Crc
