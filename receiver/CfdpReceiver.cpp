#include "CfdpReceiver.h"
#include "Crc.h"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>

CfdpReceiver::CfdpReceiver(const std::string& state_dir,
                           const std::string& recv_dir,
                           ICfdpTransport& transport)
    : journal_(state_dir)
    , recv_dir_(recv_dir)
    , transport_(transport)
{
}

// ── Boot recovery ─────────────────────────────────────────────────────────────

void CfdpReceiver::on_boot()
{
    if (!journal_.exists())
    {
        std::cout << "[receiver] no journal found, starting fresh\n";
        return;
    }

    if (!journal_.load(state_))
    {
        std::cerr << "[receiver] journal load failed (corrupted?) — starting fresh\n";
        journal_.remove();
        return;
    }

    std::cout << "[receiver] journal loaded: tid=" << state_.transaction_id
              << " file=" << state_.filename
              << " chunks=" << state_.total_chunks << "\n";

    transfer_active_     = true;
    current_chunk_index_ = UINT32_MAX;
    max_chunk_seen_      = 0;
    packet_list_.clear();

    // Don't send a NAK here — the sender isn't connected yet.
    // NAKs will be emitted by check_completion() once the first EOF arrives.
    auto missing = journal_.compute_missing_chunks(state_);
    if (missing.empty() && state_.eof_received)
    {
        // All chunks done but the final ACK was lost — resend it
        check_completion();
    }
}

// ── PDU dispatcher ────────────────────────────────────────────────────────────

void CfdpReceiver::on_pdu_received(const std::vector<uint8_t>& raw_pdu)
{
    if (raw_pdu.empty())
    {
        std::cerr << "[receiver] dropped empty PDU\n";
        return;
    }

    auto type = static_cast<PduType>(raw_pdu[0]);
    const uint8_t* payload = raw_pdu.data() + 1;

    switch (type)
    {
        case PduType::Metadata:
        {
            if (raw_pdu.size() < 1 + sizeof(MetadataPdu)) break;
            MetadataPdu meta{};
            std::memcpy(&meta, payload, sizeof(meta));
            handle_metadata(meta);
            break;
        }
        case PduType::Data:
        {
            // data[] is variable-size — deserialize the fixed header first,
            // then copy data_len bytes separately
            static constexpr size_t HDR = sizeof(DataPdu) - DEFAULT_PACKET_SIZE;
            if (raw_pdu.size() < 1 + HDR) break;
            DataPdu dpdu{};
            std::memcpy(&dpdu, payload, HDR);
            if (dpdu.data_len > DEFAULT_PACKET_SIZE) break;
            if (raw_pdu.size() < 1 + HDR + dpdu.data_len) break;
            std::memcpy(dpdu.data, payload + HDR, dpdu.data_len);
            handle_data(dpdu);
            break;
        }
        case PduType::Eof:
        {
            if (raw_pdu.size() < 1 + sizeof(EofPdu)) break;
            EofPdu eof{};
            std::memcpy(&eof, payload, sizeof(eof));
            handle_eof(eof);
            break;
        }
        case PduType::ResumeQuery:
        {
            if (raw_pdu.size() < 1 + sizeof(ResumeQueryPdu)) break;
            ResumeQueryPdu query{};
            std::memcpy(&query, payload, sizeof(query));
            handle_resume_query(query);
            break;
        }
        default:
            break;
    }
}

// ── PDU handlers ──────────────────────────────────────────────────────────────

void CfdpReceiver::handle_metadata(const MetadataPdu& meta)
{
    // Duplicate metadata for the same active transaction — ignore.
    // Don't touch max_chunk_seen_; it reflects chunks actually received.
    if (transfer_active_ && state_.transaction_id == meta.transaction_id)
    {
        std::cout << "[receiver] duplicate Metadata for tid=" << meta.transaction_id << ", ignored\n";
        return;
    }

    // Transfer already finished for this tid — resend the final ACK
    if (!transfer_active_ && last_acked_id_ == meta.transaction_id)
    {
        send_ack(UINT32_MAX);
        return;
    }

    // Sanity checks — guard against OOM and divide-by-zero
    if (meta.total_chunks == 0 || meta.total_chunks > (1u << 20))
    {
        std::cerr << "[receiver] Metadata rejected: invalid total_chunks=" << meta.total_chunks << "\n";
        return;
    }
    if (meta.packet_size == 0 || meta.packet_size > DEFAULT_PACKET_SIZE)
    {
        std::cerr << "[receiver] Metadata rejected: invalid packet_size=" << meta.packet_size << "\n";
        return;
    }
    if (meta.chunk_size == 0)
    {
        std::cerr << "[receiver] Metadata rejected: chunk_size is zero\n";
        return;
    }

    // Reject path traversal attempts (e.g. "../../etc/passwd")
    size_t fname_len = strnlen(meta.filename, sizeof(meta.filename));
    std::string fname(meta.filename, fname_len);
    if (fname.empty() || fname.find('/') != std::string::npos
                      || fname.find("..") != std::string::npos)
    {
        std::cerr << "[receiver] Metadata rejected: unsafe filename '" << fname << "'\n";
        return;
    }

    if (journal_.exists())
    {
        journal_.remove();
    }

    state_.transaction_id    = meta.transaction_id;
    state_.file_size         = meta.file_size;
    state_.chunk_size        = meta.chunk_size;
    state_.packet_size       = meta.packet_size;
    state_.total_chunks      = meta.total_chunks;
    state_.checksum_expected = meta.checksum;
    state_.eof_received      = false;

    std::strncpy(state_.filename, meta.filename, sizeof(state_.filename) - 1);
    snprintf(state_.dest_path, sizeof(state_.dest_path),
             "%s/%s", recv_dir_.c_str(), meta.filename);
    state_.chunk_done.assign(meta.total_chunks, false);

    journal_.create(state_);
    transfer_active_     = true;
    current_chunk_index_ = UINT32_MAX;
    max_chunk_seen_      = 0;
    packet_list_.clear();
}

void CfdpReceiver::handle_data(const DataPdu& dpdu)
{
    if (!transfer_active_)
    {
        std::cerr << "[receiver] Data dropped: no active transfer\n";
        return;
    }
    if (dpdu.transaction_id != state_.transaction_id)
    {
        std::cerr << "[receiver] Data dropped: tid mismatch (" << dpdu.transaction_id << " != " << state_.transaction_id << ")\n";
        return;
    }
    if (dpdu.data_len == 0 || dpdu.data_len > DEFAULT_PACKET_SIZE)
    {
        std::cerr << "[receiver] Data dropped: invalid data_len=" << dpdu.data_len << "\n";
        return;
    }
    if (dpdu.chunk_index >= state_.total_chunks)
    {
        std::cerr << "[receiver] Data dropped: chunk_index=" << dpdu.chunk_index << " out of range\n";
        return;
    }

    // Clamp total_packets against the expected value to prevent huge allocations
    uint32_t expected_pkts = (static_cast<uint32_t>(
        std::min<uint64_t>(state_.file_size - chunk_file_offset(dpdu.chunk_index),
                           state_.chunk_size))
        + state_.packet_size - 1) / state_.packet_size;
    if (dpdu.total_packets != expected_pkts)
    {
        std::cerr << "[receiver] Data dropped: total_packets mismatch (got " << dpdu.total_packets << ", expected " << expected_pkts << ")\n";
        return;
    }

    uint32_t ci = dpdu.chunk_index;
    if (ci > max_chunk_seen_)
    {
        max_chunk_seen_ = ci;
    }

    // Chunk already done — re-ACK on packet 0 so the sender can move on
    if (ci < state_.chunk_done.size() && state_.chunk_done[ci])
    {
        if (dpdu.packet_index == 0)
        {
            send_ack(ci);
        }
        return;
    }

    // New chunk — NAK the previous if still incomplete, then reset the packet list
    if (ci != current_chunk_index_)
    {
        if (current_chunk_index_ != UINT32_MAX && !packet_list_.is_complete())
        {
            send_nak_packets(current_chunk_index_);
        }
        packet_list_.clear();
        packet_list_.init(dpdu.total_packets);
        current_chunk_index_ = ci;
    }

    // Insert packet — mark valid only if CRC-8 matches
    uint8_t computed = Crc::crc8(dpdu.data, dpdu.data_len);
    bool    valid    = (computed == dpdu.crc8);
    packet_list_.insert(dpdu.packet_index, dpdu.data, dpdu.data_len, valid);

    if (packet_list_.is_complete())
    {
        flush_chunk(ci);
    }
    // NAK is sent once per EOF round by check_completion(), not per packet
}

void CfdpReceiver::handle_eof(const EofPdu& eof)
{
    if (!transfer_active_)
    {
        std::cerr << "[receiver] Eof dropped: no active transfer\n";
        return;
    }
    if (eof.transaction_id != state_.transaction_id)
    {
        std::cerr << "[receiver] Eof dropped: tid mismatch (" << eof.transaction_id << " != " << state_.transaction_id << ")\n";
        return;
    }

    // Avoid redundant journal writes on duplicate EOFs
    if (!state_.eof_received)
    {
        journal_.mark_eof_received();
        state_.eof_received = true;
    }

    check_completion();
}

// ── Chunk logic ───────────────────────────────────────────────────────────────

void CfdpReceiver::flush_chunk(uint32_t chunk_index)
{
    auto data = packet_list_.assemble();

    // Pre-allocate the output file to full size on first write
    std::fstream f(recv_file_path(),
                   std::ios::binary | std::ios::in | std::ios::out);
    if (!f)
    {
        if (state_.file_size > 0)
        {
            std::ofstream init(recv_file_path(), std::ios::binary | std::ios::trunc);
            if (!init)
            {
                throw std::runtime_error("CfdpReceiver: cannot create file: " + recv_file_path());
            }
            init.seekp(static_cast<std::streamoff>(state_.file_size - 1));
            init.put('\0');
            init.close();
            f.open(recv_file_path(), std::ios::binary | std::ios::in | std::ios::out);
        }
    }
    if (!f)
    {
        throw std::runtime_error("CfdpReceiver: cannot open file: " + recv_file_path());
    }

    uint64_t offset = chunk_file_offset(chunk_index);
    f.seekp(static_cast<std::streamoff>(offset));
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    f.flush();
    if (!f)
    {
        throw std::runtime_error("CfdpReceiver: write failed: " + recv_file_path());
    }
    f.close();

    // fdatasync before updating the journal — data must hit disk before the rename
    {
        int fd = ::open(recv_file_path().c_str(), O_RDWR);
        if (fd >= 0)
        {
            ::fdatasync(fd);
            ::close(fd);
        }
    }

    // Mark done in the journal only after the data is safely on disk
    state_.chunk_done[chunk_index] = true;
    journal_.mark_chunk_done(chunk_index);

    send_ack(chunk_index);

    packet_list_.clear();
    current_chunk_index_ = UINT32_MAX;

    if (state_.eof_received)
    {
        check_completion();
    }
}

void CfdpReceiver::check_completion()
{
    if (!state_.eof_received)
    {
        return; // nothing to do until EOF arrives
    }

    // Priority 1 — finish the current chunk first (precise packet-level NAK)
    if (current_chunk_index_ != UINT32_MAX && !packet_list_.is_complete())
    {
        send_nak_packets(current_chunk_index_);
        return;
    }

    // Priority 2 — NAK for the first missing chunk the sender has already sent.
    // We cap at max_chunk_seen_ so we never request a chunk the sender hasn't sent yet.
    auto all_missing = journal_.compute_missing_chunks(state_);
    for (const auto& cr : all_missing)
    {
        if (cr.first_chunk > max_chunk_seen_) continue;
        uint32_t ci         = cr.first_chunk;
        uint64_t chunk_off  = chunk_file_offset(ci);
        uint64_t remaining  = state_.file_size - chunk_off;
        uint32_t chunk_sz   = static_cast<uint32_t>(
                                  std::min<uint64_t>(remaining, state_.chunk_size));
        uint32_t total_pkts = (chunk_sz + state_.packet_size - 1) / state_.packet_size;

        NakPdu nak;
        nak.transaction_id = state_.transaction_id;
        nak.missing_packets.push_back({ ci, 0, total_pkts - 1 });
        transport_.send(serialize_nak(nak));
        return;
    }

    // Still waiting for chunks the sender hasn't sent yet
    if (!all_missing.empty())
    {
        return;
    }

    // Everything received — verify the full-file CRC-32
    if (!verify_file_checksum())
    {
        // File is corrupt; the sender will need to restart
        transfer_active_ = false;
        journal_.remove();
        return;
    }

    last_acked_id_ = state_.transaction_id;
    send_ack(UINT32_MAX); // final ACK
    transfer_active_ = false;
    journal_.remove();
}

// ── Outgoing PDUs ─────────────────────────────────────────────────────────────

std::vector<uint8_t> CfdpReceiver::serialize_nak(const NakPdu& nak)
{
    uint32_t count = static_cast<uint32_t>(nak.missing_packets.size());
    std::vector<uint8_t> pdu;
    pdu.reserve(1 + sizeof(nak.transaction_id) + sizeof(count)
                  + count * sizeof(PacketNakRange));
    pdu.push_back(static_cast<uint8_t>(PduType::Nak));

    auto append = [&](const void* ptr, size_t sz)
    {
        const auto* p = static_cast<const uint8_t*>(ptr);
        pdu.insert(pdu.end(), p, p + sz);
    };

    append(&nak.transaction_id, sizeof(nak.transaction_id));
    append(&count, sizeof(count));
    for (const auto& r : nak.missing_packets)
    {
        append(&r, sizeof(r));
    }
    return pdu;
}

void CfdpReceiver::send_nak_packets(uint32_t chunk_index)
{
    auto nak_ranges = packet_list_.compute_nak_ranges();
    if (nak_ranges.empty())
    {
        return;
    }

    NakPdu nak;
    nak.transaction_id = state_.transaction_id;
    for (const auto& [first, last] : nak_ranges)
    {
        nak.missing_packets.push_back({ chunk_index, first, last });
    }
    transport_.send(serialize_nak(nak));
}

void CfdpReceiver::handle_resume_query(const ResumeQueryPdu& query)
{
    ResumeInfoPdu info{};
    info.transaction_id = query.transaction_id;

    if (!transfer_active_ || state_.transaction_id != query.transaction_id)
    {
        // No active transfer for this tid — start from scratch
        info.first_missing_chunk = 0;
    }
    else
    {
        auto missing = journal_.compute_missing_chunks(state_);
        if (missing.empty())
        {
            // All chunks received, final ACK was probably lost
            info.first_missing_chunk = UINT32_MAX;
        }
        else
        {
            info.first_missing_chunk = missing[0].first_chunk;
        }
    }

    std::vector<uint8_t> pdu(1 + sizeof(ResumeInfoPdu));
    pdu[0] = static_cast<uint8_t>(PduType::ResumeInfo);
    std::memcpy(pdu.data() + 1, &info, sizeof(info));
    transport_.send(pdu);
}

void CfdpReceiver::send_ack(uint32_t chunk_index)
{
    AckPdu ack{ state_.transaction_id, chunk_index };
    std::vector<uint8_t> pdu(1 + sizeof(AckPdu));
    pdu[0] = static_cast<uint8_t>(PduType::Ack);
    std::memcpy(pdu.data() + 1, &ack, sizeof(ack));
    transport_.send(pdu);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

bool CfdpReceiver::verify_file_checksum()
{
    uint32_t computed = Crc::crc32_file(recv_file_path());
    return computed == state_.checksum_expected;
}

std::string CfdpReceiver::recv_file_path() const
{
    return std::string(state_.dest_path);
}

uint64_t CfdpReceiver::chunk_file_offset(uint32_t ci) const
{
    return static_cast<uint64_t>(ci) * state_.chunk_size;
}
