#include "PacketList.h"
#include <stdexcept>
#include <ostream>
#include <iostream>

PacketList::~PacketList() {
    clear();
}

void PacketList::init(uint32_t total_packets) {
    clear();

    total_packets_ = total_packets;
    valid_count_   = 0;
    index_.resize(total_packets, nullptr);

    if (total_packets == 0) return;

    // Allocate all nodes and chain them
    for (uint32_t i = 0; i < total_packets; ++i) {
        PacketNode* node = new PacketNode();
        node->packet_index = i;

        if (head_ == nullptr) {
            head_ = node;
            tail_ = node;
        } else {
            tail_->next = node;
            node->prev  = tail_;
            tail_       = node;
        }

        index_[i] = node;
    }
}

void PacketList::clear() {
    PacketNode* cur = head_;
    while (cur) {
        PacketNode* next = cur->next;
        delete cur;
        cur = next;
    }
    head_           = nullptr;
    tail_           = nullptr;
    total_packets_  = 0;
    valid_count_    = 0;
    index_.clear();
}

bool PacketList::insert(uint32_t packet_index,
                        const uint8_t* data, uint32_t len,
                        bool valid)
{
    if (packet_index >= total_packets_) return false;

    PacketNode* node = index_[packet_index];
    if (!node) return false;

    // Track whether the slot was already valid to keep valid_count_ accurate
    bool was_valid = node->received && node->valid;

    node->received = true;
    node->valid    = valid;
    node->data.assign(data, data + len);

    if (valid && !was_valid) {
        ++valid_count_;
    } else if (!valid && was_valid) {
        // Retransmitted but still corrupt
        --valid_count_;
    }

    return true;
}

bool PacketList::is_complete() const {
    return (total_packets_ > 0) && (valid_count_ == total_packets_);
}

std::vector<std::pair<uint32_t,uint32_t>> PacketList::compute_nak_ranges() const {
    std::vector<std::pair<uint32_t,uint32_t>> ranges;

    int range_start = -1;

    for (uint32_t i = 0; i < total_packets_; ++i) {
        PacketNode* node = index_[i];
        bool needs_retransmit = !node->received || !node->valid;

        if (needs_retransmit && range_start == -1)
        {
            range_start = static_cast<int>(i);
        } else if (!needs_retransmit && range_start != -1)
        {
            ranges.push_back({ static_cast<uint32_t>(range_start), i - 1 });
            range_start = -1;
        }
    }

    if (range_start != -1) {
        ranges.push_back({ static_cast<uint32_t>(range_start), total_packets_ - 1 });
    }

    return ranges;
}

std::vector<uint8_t> PacketList::assemble() const {
    std::vector<uint8_t> result;
    result.reserve(total_packets_ * 1024);

    PacketNode* cur = head_;
    while (cur) {
        result.insert(result.end(), cur->data.begin(), cur->data.end());
        cur = cur->next;
    }

    return result;
}
