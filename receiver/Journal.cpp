#include "Journal.h"
#include <fstream>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

Journal::Journal(const std::string& state_dir)
    : state_path_(state_dir + "/state.bin")
    , tmp_path_  (state_dir + "/state.bin.tmp")
{}

bool Journal::exists() const {
    struct stat st;
    return stat(state_path_.c_str(), &st) == 0;
}

bool Journal::create(const JournalState& state) {
    return atomic_write(state);
}

bool Journal::mark_chunk_done(uint32_t chunk_index) {
    JournalState state;
    if (!read_from_disk(state)) return false;
    if (chunk_index >= state.chunk_done.size()) return false;
    state.chunk_done[chunk_index] = true;
    return atomic_write(state);
}

bool Journal::mark_eof_received() {
    JournalState state;
    if (!read_from_disk(state)) return false;
    state.eof_received = true;
    return atomic_write(state);
}

bool Journal::load(JournalState& out_state) {
    return read_from_disk(out_state);
}

bool Journal::remove() {
    return std::remove(state_path_.c_str()) == 0;
}

std::vector<ChunkNakRange> Journal::compute_missing_chunks(const JournalState& state) const {
    std::vector<ChunkNakRange> ranges;
    int range_start = -1;

    for (uint32_t i = 0; i < state.total_chunks; ++i) {
        bool missing = (i >= state.chunk_done.size()) || !state.chunk_done[i];

        if (missing && range_start == -1) {
            range_start = static_cast<int>(i);
        } else if (!missing && range_start != -1) {
            ranges.push_back({ static_cast<uint32_t>(range_start), i - 1 });
            range_start = -1;
        }
    }
    if (range_start != -1) {
        ranges.push_back({ static_cast<uint32_t>(range_start), state.total_chunks - 1 });
    }

    return ranges;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Sérialisation binaire simple
//  À remplacer par flatbuffers / CBOR / protobuf selon contraintes
// ─────────────────────────────────────────────────────────────────────────────

bool Journal::atomic_write(const JournalState& s) {
    std::ofstream f(tmp_path_, std::ios::binary | std::ios::trunc);
    if (!f) return false;

    auto w = [&](const void* ptr, size_t sz) {
        f.write(static_cast<const char*>(ptr), static_cast<std::streamsize>(sz));
    };

    w(&s.transaction_id,    sizeof(s.transaction_id));
    w(&s.file_size,         sizeof(s.file_size));
    w(&s.chunk_size,        sizeof(s.chunk_size));
    w(&s.packet_size,       sizeof(s.packet_size));
    w(&s.total_chunks,      sizeof(s.total_chunks));
    w(&s.checksum_expected, sizeof(s.checksum_expected));
    w(&s.eof_received,      sizeof(s.eof_received));
    w(s.filename,           sizeof(s.filename));
    w(s.dest_path,          sizeof(s.dest_path));

    uint32_t n = static_cast<uint32_t>(s.chunk_done.size());
    w(&n, sizeof(n));
    for (bool b : s.chunk_done) {
        uint8_t v = b ? 1u : 0u;
        w(&v, sizeof(v));
    }

    f.flush();
    f.close();
    if (!f) return false;

    return std::rename(tmp_path_.c_str(), state_path_.c_str()) == 0;
}

bool Journal::read_from_disk(JournalState& s) {
    std::ifstream f(state_path_, std::ios::binary);
    if (!f) return false;

    auto r = [&](void* ptr, size_t sz) {
        f.read(static_cast<char*>(ptr), static_cast<std::streamsize>(sz));
    };

    r(&s.transaction_id,    sizeof(s.transaction_id));
    r(&s.file_size,         sizeof(s.file_size));
    r(&s.chunk_size,        sizeof(s.chunk_size));
    r(&s.packet_size,       sizeof(s.packet_size));
    r(&s.total_chunks,      sizeof(s.total_chunks));
    r(&s.checksum_expected, sizeof(s.checksum_expected));
    r(&s.eof_received,      sizeof(s.eof_received));
    r(s.filename,           sizeof(s.filename));
    r(s.dest_path,          sizeof(s.dest_path));

    uint32_t n = 0;
    r(&n, sizeof(n));
    s.chunk_done.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t v = 0;
        r(&v, sizeof(v));
        s.chunk_done[i] = (v != 0);
    }

    return f.good();
}
