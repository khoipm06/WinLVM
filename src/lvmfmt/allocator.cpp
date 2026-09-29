#include "lvmfmt/allocator.h"
#include <cctype>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace lvmfmt {

std::optional<std::uint64_t> parse_size_bytes(std::string_view str, std::string& error) {
    if (str.empty()) {
        error = "empty size string";
        return std::nullopt;
    }
    // Trim leading/trailing whitespace
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.front()))) str.remove_prefix(1);
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back()))) str.remove_suffix(1);
    if (str.empty()) {
        error = "empty size string";
        return std::nullopt;
    }

    // Skip optional leading '+' or '-'
    if (str.front() == '+' || str.front() == '-') {
        str.remove_prefix(1);
    }

    char unit = 'B';
    size_t num_end = 0;
    while (num_end < str.size() && (std::isdigit(static_cast<unsigned char>(str[num_end])) || str[num_end] == '.')) {
        num_end++;
    }
    if (num_end == 0) {
        error = "no numeric digits in size string: " + std::string(str);
        return std::nullopt;
    }

    double value = 0.0;
    try {
        value = std::stod(std::string(str.substr(0, num_end)));
    } catch (...) {
        error = "failed to parse numeric value: " + std::string(str.substr(0, num_end));
        return std::nullopt;
    }

    if (num_end < str.size()) {
        unit = static_cast<char>(std::toupper(static_cast<unsigned char>(str[num_end])));
    }

    std::uint64_t multiplier = 1;
    switch (unit) {
        case 'S': multiplier = 512ULL; break; // sectors
        case 'K': multiplier = 1024ULL; break;
        case 'M': multiplier = 1024ULL * 1024ULL; break;
        case 'G': multiplier = 1024ULL * 1024ULL * 1024ULL; break;
        case 'T': multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL; break;
        case 'P': multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL; break;
        case 'B': multiplier = 1; break;
        default:
            error = "unknown size unit suffix: " + std::string(1, unit);
            return std::nullopt;
    }

    double total = value * static_cast<double>(multiplier);
    if (total < 0.0 || total > static_cast<double>(UINT64_MAX)) {
        error = "size out of bounds";
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(total);
}

std::string format_size_human(std::uint64_t bytes) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    if (bytes >= 1024ULL * 1024ULL * 1024ULL * 1024ULL) {
        ss << (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0 * 1024.0)) << " TiB";
    } else if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        ss << (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) << " GiB";
    } else if (bytes >= 1024ULL * 1024ULL) {
        ss << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MiB";
    } else if (bytes >= 1024ULL) {
        ss << (static_cast<double>(bytes) / 1024.0) << " KiB";
    } else {
        ss << bytes << " B";
    }
    return ss.str();
}

std::vector<PvExtentMap> build_vg_extent_maps(const VolumeGroup& vg) {
    std::vector<PvExtentMap> maps;
    for (const auto& pv : vg.pvs) {
        PvExtentMap m;
        m.pv_key = pv.key;
        m.pv_uuid = pv.id;
        m.total_extents = pv.pe_count;
        m.extent_used.resize(pv.pe_count, false);
        m.free_extents = pv.pe_count;
        maps.push_back(std::move(m));
    }

    // Mark used extents by traversing all segments of all LVs
    for (const auto& lv : vg.lvs) {
        for (const auto& seg : lv.segments) {
            for (const auto& stripe : seg.stripes) {
                // Find matching PV in maps
                for (auto& m : maps) {
                    if (m.pv_key == stripe.target) {
                        for (std::uint64_t e = 0; e < seg.extent_count; e++) {
                            std::uint64_t pe = stripe.start_extent + e;
                            if (pe < m.total_extents && !m.extent_used[pe]) {
                                m.extent_used[pe] = true;
                                if (m.free_extents > 0) m.free_extents--;
                            }
                        }
                    }
                }
            }
        }
    }
    return maps;
}

bool allocate_linear(const VolumeGroup& vg,
                     std::uint64_t extents_needed,
                     const std::vector<std::string>& preferred_pvs,
                     std::vector<ExtentAllocation>& out_allocations,
                     std::string& error) {
    out_allocations.clear();
    if (extents_needed == 0) return true;

    auto maps = build_vg_extent_maps(vg);
    std::uint64_t remaining = extents_needed;

    // Filter maps by preferred_pvs if specified
    std::vector<size_t> candidate_indices;
    if (!preferred_pvs.empty()) {
        for (const auto& pref : preferred_pvs) {
            for (size_t i = 0; i < maps.size(); i++) {
                if (maps[i].pv_key == pref || maps[i].pv_uuid == pref) {
                    candidate_indices.push_back(i);
                    break;
                }
            }
        }
    } else {
        for (size_t i = 0; i < maps.size(); i++) candidate_indices.push_back(i);
    }

    for (size_t idx : candidate_indices) {
        auto& m = maps[idx];
        if (m.free_extents == 0) continue;

        std::uint64_t pe = 0;
        while (pe < m.total_extents && remaining > 0) {
            if (!m.extent_used[pe]) {
                std::uint64_t run_start = pe;
                std::uint64_t run_len = 0;
                while (pe < m.total_extents && !m.extent_used[pe] && remaining > 0) {
                    m.extent_used[pe] = true;
                    m.free_extents--;
                    run_len++;
                    remaining--;
                    pe++;
                }
                out_allocations.push_back({m.pv_key, run_start, run_len});
            } else {
                pe++;
            }
        }
        if (remaining == 0) break;
    }

    if (remaining > 0) {
        error = "insufficient free extents in volume group: requested " +
                std::to_string(extents_needed) + ", short by " + std::to_string(remaining);
        return false;
    }
    return true;
}

bool allocate_striped(const VolumeGroup& vg,
                      std::uint64_t extents_needed,
                      std::uint32_t stripe_count,
                      const std::vector<std::string>& preferred_pvs,
                      std::vector<std::vector<ExtentAllocation>>& out_stripe_allocations,
                      std::string& error) {
    out_stripe_allocations.clear();
    if (stripe_count == 0) {
        error = "stripe count must be >= 1";
        return false;
    }
    if (extents_needed % stripe_count != 0) {
        error = "extent count (" + std::to_string(extents_needed) +
                ") must be a multiple of stripe count (" + std::to_string(stripe_count) + ")";
        return false;
    }

    auto maps = build_vg_extent_maps(vg);
    if (maps.size() < stripe_count) {
        error = "volume group only has " + std::to_string(maps.size()) +
                " PVs, but " + std::to_string(stripe_count) + " stripes requested";
        return false;
    }

    std::uint64_t extents_per_stripe = extents_needed / stripe_count;
    out_stripe_allocations.resize(stripe_count);

    for (std::uint32_t s = 0; s < stripe_count; s++) {
        std::vector<std::string> single_pv;
        if (s < preferred_pvs.size()) {
            single_pv.push_back(preferred_pvs[s]);
        } else {
            single_pv.push_back(maps[s].pv_key);
        }
        if (!allocate_linear(vg, extents_per_stripe, single_pv, out_stripe_allocations[s], error)) {
            return false;
        }
    }
    return true;
}

bool extend_lv(VolumeGroup& vg,
               std::string_view lv_name,
               std::uint64_t additional_extents,
               const std::vector<std::string>& preferred_pvs,
               std::string& error) {
    LogicalVolume* target = nullptr;
    for (auto& lv : vg.lvs) {
        if (lv.name == lv_name) {
            target = &lv;
            break;
        }
    }
    if (!target) {
        error = "logical volume not found: " + std::string(lv_name);
        return false;
    }
    if (additional_extents == 0) return true;

    std::vector<ExtentAllocation> allocs;
    if (!allocate_linear(vg, additional_extents, preferred_pvs, allocs, error)) {
        return false;
    }

    std::uint64_t cur_le = target->extent_count();
    for (const auto& a : allocs) {
        Segment seg;
        seg.start_extent = cur_le;
        seg.extent_count = a.count;
        seg.type = "linear";
        seg.stripe_count = 1;
        seg.stripe_size_sectors = 0;
        seg.stripes.push_back({a.pv_key, a.start_pe});
        target->segments.push_back(std::move(seg));
        cur_le += a.count;
    }
    vg.seqno++;
    return true;
}

bool reduce_lv(VolumeGroup& vg,
               std::string_view lv_name,
               std::uint64_t extents_to_remove,
               std::string& error) {
    LogicalVolume* target = nullptr;
    for (auto& lv : vg.lvs) {
        if (lv.name == lv_name) {
            target = &lv;
            break;
        }
    }
    if (!target) {
        error = "logical volume not found: " + std::string(lv_name);
        return false;
    }

    std::uint64_t total = target->extent_count();
    if (extents_to_remove >= total) {
        error = "cannot reduce LV by " + std::to_string(extents_to_remove) +
                " extents; total size is only " + std::to_string(total) + " extents";
        return false;
    }

    std::uint64_t target_extents = total - extents_to_remove;
    std::uint64_t accumulated = 0;
    std::vector<Segment> kept_segments;

    for (auto& seg : target->segments) {
        if (accumulated + seg.extent_count <= target_extents) {
            accumulated += seg.extent_count;
            kept_segments.push_back(std::move(seg));
        } else if (accumulated < target_extents) {
            std::uint64_t keep = target_extents - accumulated;
            seg.extent_count = keep;
            accumulated += keep;
            kept_segments.push_back(std::move(seg));
            break;
        } else {
            break;
        }
    }
    target->segments = std::move(kept_segments);
    vg.seqno++;
    return true;
}

bool move_pv_extents(VolumeGroup& vg,
                     std::string_view source_pv_key_or_name,
                     std::string_view dest_pv_key_or_name,
                     std::optional<std::string_view> target_lv_name,
                     ByteSource* source_storage,
                     ByteSource* dest_storage,
                     std::string& error) {
    // Resolve PV keys
    const PhysicalVolume* src_pv = vg.find_pv(source_pv_key_or_name);
    if (!src_pv) src_pv = vg.find_pv_by_uuid(source_pv_key_or_name);
    if (!src_pv) {
        error = "source PV not found in VG: " + std::string(source_pv_key_or_name);
        return false;
    }

    const PhysicalVolume* dst_pv = vg.find_pv(dest_pv_key_or_name);
    if (!dst_pv) dst_pv = vg.find_pv_by_uuid(dest_pv_key_or_name);
    if (!dst_pv) {
        error = "destination PV not found in VG: " + std::string(dest_pv_key_or_name);
        return false;
    }

    if (src_pv->key == dst_pv->key) {
        error = "source and destination PV cannot be the same";
        return false;
    }

    auto maps = build_vg_extent_maps(vg);
    size_t dst_idx = 0;
    for (size_t i = 0; i < maps.size(); i++) {
        if (maps[i].pv_key == dst_pv->key) {
            dst_idx = i;
            break;
        }
    }

    std::uint64_t extent_bytes = vg.extent_size_sectors * 512ULL;
    std::vector<uint8_t> buffer(64 * 1024); // 64 KiB transfer block

    // Iterate through all LVs and segments using src_pv
    for (auto& lv : vg.lvs) {
        if (target_lv_name && lv.name != *target_lv_name) continue;

        for (auto& seg : lv.segments) {
            for (auto& stripe : seg.stripes) {
                if (stripe.target == src_pv->key) {
                    std::uint64_t count = seg.extent_count;
                    // Allocate `count` extents on dst_pv
                    std::uint64_t dst_pe = 0;
                    bool allocated = false;
                    for (std::uint64_t p = 0; p + count <= maps[dst_idx].total_extents; p++) {
                        bool run_free = true;
                        for (std::uint64_t k = 0; k < count; k++) {
                            if (maps[dst_idx].extent_used[p + k]) {
                                run_free = false;
                                p += k;
                                break;
                            }
                        }
                        if (run_free) {
                            dst_pe = p;
                            for (std::uint64_t k = 0; k < count; k++) {
                                maps[dst_idx].extent_used[dst_pe + k] = true;
                            }
                            allocated = true;
                            break;
                        }
                    }

                    if (!allocated) {
                        error = "destination PV " + dst_pv->key + " has insufficient contiguous extents";
                        return false;
                    }

                    // Copy underlying data if storage sources are provided
                    if (source_storage && dest_storage) {
                        std::uint64_t src_byte_offset = (src_pv->pe_start_sectors * 512ULL) +
                                                        (stripe.start_extent * extent_bytes);
                        std::uint64_t dst_byte_offset = (dst_pv->pe_start_sectors * 512ULL) +
                                                        (dst_pe * extent_bytes);
                        std::uint64_t bytes_to_copy = count * extent_bytes;

                        while (bytes_to_copy > 0) {
                            size_t chunk = static_cast<size_t>(std::min<std::uint64_t>(bytes_to_copy, buffer.size()));
                            if (!source_storage->read(src_byte_offset, buffer.data(), chunk, error)) {
                                error = "pvmove read error at offset " + std::to_string(src_byte_offset) + ": " + error;
                                return false;
                            }
                            if (!dest_storage->write(dst_byte_offset, buffer.data(), chunk, error)) {
                                error = "pvmove write error at offset " + std::to_string(dst_byte_offset) + ": " + error;
                                return false;
                            }
                            src_byte_offset += chunk;
                            dst_byte_offset += chunk;
                            bytes_to_copy -= chunk;
                        }
                    }

                    // Update stripe target and extent
                    stripe.target = dst_pv->key;
                    stripe.start_extent = dst_pe;
                }
            }
        }
    }

    vg.seqno++;
    return true;
}

} // namespace lvmfmt
