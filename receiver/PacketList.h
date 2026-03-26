#pragma once
#include "Types.h"
#include <vector>
#include <cstdint>

// Doubly-linked list of packet slots for one chunk.
// Allocated once per chunk with total_packets nodes.
//
// Node states:
//   received=false              → never arrived
//   received=true, valid=true   → arrived, CRC-8 OK
//   received=true, valid=false  → arrived but corrupted → needs retransmit
//
// O(1) insert via direct index array — no memory shifting.

struct PacketNode {
    uint32_t             packet_index;
    bool                 received{false};
    bool                 valid{false};  // CRC-8 passed
    std::vector<uint8_t> data;

    PacketNode* prev{nullptr};
    PacketNode* next{nullptr};
};

class PacketList {
public:
    PacketList() = default;
    ~PacketList();

    // No copy — raw pointers
    PacketList(const PacketList&)            = delete;
    PacketList& operator=(const PacketList&) = delete;

    void init(uint32_t total_packets);  // allocate all nodes
    void clear();                       // free everything

    // Insert packet data into the right slot; returns false if index out of range
    bool insert(uint32_t packet_index, const uint8_t* data, uint32_t len, bool valid);

    // True when every slot is received and valid
    bool is_complete() const;

    // Returns contiguous ranges of missing/corrupt packets (for building a NAK)
    std::vector<std::pair<uint32_t,uint32_t>> compute_nak_ranges() const;

    // Concatenate all packets in order — only call when is_complete()
    std::vector<uint8_t> assemble() const;

    uint32_t total_packets() const { return total_packets_; }

private:
    PacketNode* head_{nullptr};
    PacketNode* tail_{nullptr};
    uint32_t    total_packets_{0};
    uint32_t    valid_count_{0};

    // Direct O(1) access by packet index
    std::vector<PacketNode*> index_;
};
