#pragma once
#include "Types.h"
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  Format wire des PDU satellite → ground (payload après le type byte)
// ─────────────────────────────────────────────────────────────────────────────

struct NakHeader {
    uint32_t transaction_id;
    uint32_t count;
} __attribute__((packed));

struct AckHeader {
    uint32_t transaction_id;
    uint32_t chunk_index;   // UINT32_MAX = ACK final
} __attribute__((packed));

// ─────────────────────────────────────────────────────────────────────────────

inline bool parse_nak(const std::vector<uint8_t>& pdu, NakPdu& out) {
    if (pdu.size() < 1 + sizeof(NakHeader)) return false;
    const auto* hdr    = reinterpret_cast<const NakHeader*>(pdu.data() + 1);
    if (pdu.size() < 1 + sizeof(NakHeader) + static_cast<size_t>(hdr->count) * sizeof(PacketNakRange))
        return false;
    const auto* ranges = reinterpret_cast<const PacketNakRange*>(pdu.data() + 1 + sizeof(NakHeader));
    out.transaction_id = hdr->transaction_id;
    out.missing_packets.assign(ranges, ranges + hdr->count);
    return true;
}

inline bool parse_ack(const std::vector<uint8_t>& pdu, AckPdu& out) {
    if (pdu.size() < 1 + sizeof(AckHeader)) return false;
    const auto* hdr    = reinterpret_cast<const AckHeader*>(pdu.data() + 1);
    out.transaction_id = hdr->transaction_id;
    out.chunk_index    = hdr->chunk_index;
    return true;
}
