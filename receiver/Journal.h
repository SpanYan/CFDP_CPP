#pragma once
#include "Types.h"
#include <string>
#include <vector>

// Persisted transfer state — chunk granularity.
// A chunk in progress at reboot is considered lost and will be re-requested.
// Writes are atomic: write to a tmp file, then rename (Linux guarantees atomicity).
struct JournalState {
    uint32_t transaction_id;
    uint64_t file_size;
    uint32_t chunk_size;
    uint32_t packet_size;
    uint32_t total_chunks;
    uint32_t checksum_expected;
    bool     eof_received;
    char     filename[256];
    char     dest_path[512];

    // true once the chunk is fully written and fdatasync'd to disk
    std::vector<bool> chunk_done;
};

class Journal {
public:
    explicit Journal(const std::string& state_dir);

    bool create(const JournalState& state);           // call on Metadata PDU
    bool mark_chunk_done(uint32_t chunk_index);       // atomic rename
    bool mark_eof_received();
    bool load(JournalState& out_state);
    bool remove();
    bool exists() const;

    // Returns ranges of chunks not yet marked done
    std::vector<ChunkNakRange> compute_missing_chunks(const JournalState& state) const;

private:
    std::string state_path_;
    std::string tmp_path_;

    bool atomic_write(const JournalState& state);
    bool read_from_disk(JournalState& out_state);
};
