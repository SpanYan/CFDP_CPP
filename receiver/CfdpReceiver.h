#pragma once
#include "Types.h"
#include "Transport.h"
#include "Journal.h"
#include "PacketList.h"
#include <string>
#include <vector>

// CFDP Class 2 receiver — two levels of reliability:
//
//   Packet level (PacketList)
//     Missing or corrupt packets (CRC-8 fail) are tracked per slot.
//     A NAK with precise ranges is sent; retransmitted packets slot in O(1).
//
//   Chunk level (Journal)
//     chunk_done[i] is persisted atomically after each flush.
//     On reboot the journal is loaded and the sender resumes from the first missing chunk.

class CfdpReceiver {
public:
    CfdpReceiver(const std::string& state_dir,
                 const std::string& recv_dir,
                 ICfdpTransport& transport);

    // Call at startup — resumes an interrupted transfer if a journal is present
    void on_boot();

    // Feed every raw PDU received from the transport layer here
    void on_pdu_received(const std::vector<uint8_t>& raw_pdu);

    bool transfer_active() const { return transfer_active_; }

private:
    Journal         journal_;
    std::string     recv_dir_;
    ICfdpTransport& transport_;

    JournalState state_;
    bool         transfer_active_{false};
    uint32_t     last_acked_id_{0};

    // Active packet list for the current chunk (one chunk at a time)
    uint32_t    current_chunk_index_{UINT32_MAX}; // UINT32_MAX = no chunk in progress
    uint32_t    max_chunk_seen_{0};               // highest chunk index seen so far
    PacketList  packet_list_;

    // PDU handlers
    void handle_metadata    (const MetadataPdu&    meta);
    void handle_data        (const DataPdu&        dpdu);
    void handle_eof         (const EofPdu&         eof);
    void handle_resume_query(const ResumeQueryPdu& query);

    // Assemble packets, write chunk to disk, update journal
    void flush_chunk(uint32_t chunk_index);

    // Check if everything is done; send NAK or final ACK accordingly
    void check_completion();

    bool verify_file_checksum();

    std::vector<uint8_t> serialize_nak(const NakPdu& nak);
    void send_nak_packets(uint32_t chunk_index); // NAK for missing packets in current chunk
    void send_ack(uint32_t chunk_index);         // UINT32_MAX = final ACK

    std::string recv_file_path()              const;
    uint64_t    chunk_file_offset(uint32_t ci) const;
};
