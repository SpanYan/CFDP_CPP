#pragma once
#include <cstdint>
#include <vector>
#include <string>

// Default sizes — chunk is the journal unit, packet is the transport unit
static constexpr uint32_t DEFAULT_CHUNK_SIZE  = 1024u * 1024u; // 1 MB
static constexpr uint32_t DEFAULT_PACKET_SIZE = 4096u;          // 4 KB

enum class PduType : uint8_t {
    Metadata    = 0x01,
    Data        = 0x02,
    Eof         = 0x03,
    Nak         = 0x04,
    Ack         = 0x05,
    ResumeQuery = 0x06,  // sender asks: "where are you on this tid?"
    ResumeInfo  = 0x07,  // receiver replies: "resume from this chunk"
};

// Sent once at the start of a transfer
struct __attribute__((packed)) MetadataPdu {
    uint32_t transaction_id;
    uint64_t file_size;
    uint32_t chunk_size;
    uint32_t packet_size;
    uint32_t total_chunks;
    uint32_t checksum;      // CRC-32 of the full file
    char     filename[256];
};

// One packet — a slice of a chunk
struct __attribute__((packed)) DataPdu {
    uint32_t transaction_id;
    uint32_t chunk_index;
    uint32_t packet_index;
    uint32_t total_packets; // total packets in this chunk
    uint32_t data_len;      // actual payload size
    uint8_t  crc8;          // CRC-8 over data[]
    uint8_t  data[DEFAULT_PACKET_SIZE];
};

struct EofPdu {
    uint32_t transaction_id;
    uint32_t checksum;      // CRC-32 of the full file
};

// A contiguous range of missing/corrupt packets within a chunk
struct PacketNakRange {
    uint32_t chunk_index;
    uint32_t first_packet;  // inclusive
    uint32_t last_packet;   // inclusive
};

struct NakPdu {
    uint32_t                    transaction_id;
    std::vector<PacketNakRange> missing_packets;
};

// Chunk ACK — UINT32_MAX means the whole transfer is done
struct AckPdu {
    uint32_t transaction_id;
    uint32_t chunk_index;
};

// Range of missing chunks (used by the journal)
struct ChunkNakRange {
    uint32_t first_chunk;
    uint32_t last_chunk;
};

// ResumeQuery/ResumeInfo handshake at the start of a session.
// first_missing_chunk:
//   0          → no transfer in progress, start from scratch
//   N          → resume from chunk N
//   UINT32_MAX → all chunks received, just resend EOF
struct __attribute__((packed)) ResumeQueryPdu {
    uint32_t transaction_id;
};

struct __attribute__((packed)) ResumeInfoPdu {
    uint32_t transaction_id;
    uint32_t first_missing_chunk;
};
