// (PS5) A prepare-once's program records (ps5_vk_prepare.inc), held in memory of our own and written to longshot_programs.bin
// 8 MB at a time. Each record written at once into the console's title folder was its write calls and a flush: PS5_Hookshot
// measured 157 ms a record under the cache's lock, six workers at 20% of one core between them, 2.5 programs a second
// (2026-11-13, its 34d44c5f). Play writes each record at once, as gpu/vk/vk_progcache.cpp program_cache_save does.
// A held record is not in the file yet, so it is listed (disk_records) only once it is written: no read looks for it before.
// What a start that ends with records held loses (8 MB at most) is translated again at the next one.
#include "vk_progcache.h"

namespace mkwvk {

// (vk_progcache.cpp's own, not in its header)
extern FILE* disk_out;
extern FILE* disk_idx;
extern bool disk_zstd;
void idx_put(std::vector<uint8_t>& o, uint64_t key, uint64_t size, uint32_t kind);

namespace {
constexpr size_t kHold = 8u << 20;
std::atomic<bool> hold_on{false};
std::vector<uint8_t> held, held_idx;   // (the records, and their index entries: disk_mu)
std::vector<std::pair<uint64_t, DiskRecord>> held_list;   // (listed when written)
int64_t held_base = -1;   // (where the first held record goes in the file)

void put(std::vector<uint8_t>& v, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    v.insert(v.end(), b, b + n);
}
void flush_locked() {   // (disk_mu held: the records before their index entries, as program_cache_save writes them)
    if (held.empty()) return;
    if (disk_out) {
        _fseeki64(disk_out, 0, SEEK_END);
        fwrite(held.data(), 1, held.size(), disk_out);
        fflush(disk_out);
    }
    if (disk_idx && !held_idx.empty()) {
        fwrite(held_idx.data(), 1, held_idx.size(), disk_idx);
        fflush(disk_idx);
    }
    if (disk_listing)
        for (const auto& [key, d] : held_list) disk_records[key].push_back(d);
    held.clear(), held_idx.clear(), held_list.clear();
    held_base = -1;
}
}  // namespace

/// On: the records are held from now on. Off: what is held is written, and listed
void ps5_program_hold(bool on) {
    std::lock_guard lk(disk_mu);
    hold_on.store(on);
    if (!on) {
        flush_locked();
        held = {}, held_idx = {}, held_list = {};
    }
}

/// (program_cache_save, before it writes) true: the record is held, nothing more to do. The record as that function writes it:
/// key and stored size, the kind word in the zstd cache, the plain size of a compressed record (usize, 0 for none), its bytes
bool ps5_program_hold_save(uint64_t key, uint64_t stored, uint32_t kind, const std::vector<uint8_t>& payload, uint64_t usize) {
    if (!hold_on.load(std::memory_order_relaxed)) return false;
    std::lock_guard lk(disk_mu);
    if (!hold_on.load() || !disk_out) return false;
    if (held_base < 0) {
        _fseeki64(disk_out, 0, SEEK_END);
        held_base = _ftelli64(disk_out);
    }
    const int64_t at = held_base + (int64_t)held.size();
    const uint64_t head[2] = {key, stored};
    put(held, head, 16);
    if (disk_zstd) put(held, &kind, 4);
    if (usize) put(held, &usize, 8);
    put(held, payload.data(), payload.size());
    if (disk_idx) idx_put(held_idx, key, stored, kind);
    held_list.push_back({key, DiskRecord{at + (disk_zstd ? 20 : 16), (size_t)stored, kind}});
    if (held.size() >= kHold) flush_locked();
    return true;
}

}  // namespace mkwvk
