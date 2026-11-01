#include "lv_extent_alloc.h"
#include "lvmfmt/allocator.h"
#include <cstdint>
#include <string>
#include <vector>

namespace lvmfmt {

bool build_linear_segments(const VolumeGroup& vg,
                           std::uint64_t extents_needed,
                           std::vector<Segment>& out_segments,
                           std::string& error) {
    out_segments.clear();
    if (extents_needed == 0) {
        error = "requested extent count must be > 0";
        return false;
    }

    std::vector<ExtentAllocation> allocs;
    if (!allocate_linear(vg, extents_needed, {}, allocs, error)) {
        return false;
    }

    std::vector<Segment> segs;
    segs.reserve(allocs.size());
    std::uint64_t cur_le = 0;
    for (const auto& a : allocs) {
        Segment seg;
        seg.start_extent = cur_le;
        seg.extent_count = a.count;
        seg.type = "linear";
        seg.stripe_count = 1;
        seg.stripe_size_sectors = 0;
        seg.stripes.push_back({a.pv_key, a.start_pe});
        segs.push_back(std::move(seg));
        cur_le += a.count;
    }
    out_segments = std::move(segs);
    return true;
}

bool build_striped_segments(const VolumeGroup& vg,
                            std::uint64_t extents_needed,
                            std::uint32_t stripe_count,
                            std::uint32_t stripe_size_sectors,
                            std::vector<Segment>& out_segments,
                            std::string& error) {
    out_segments.clear();
    if (stripe_count == 0) {
        error = "stripe count must be >= 1";
        return false;
    }
    if (stripe_count > 1 && stripe_size_sectors == 0) {
        error = "stripe size must be > 0 sectors";
        return false;
    }
    if (extents_needed == 0) {
        error = "requested extent count must be > 0";
        return false;
    }

    std::vector<std::vector<ExtentAllocation>> stripe_allocs;
    if (!allocate_striped(vg, extents_needed, stripe_count, {}, stripe_allocs, error)) {
        return false;
    }

    std::vector<Segment> segs;
    std::vector<std::size_t> chunk_idx(stripe_count, 0);
    std::vector<std::uint64_t> chunk_off(stripe_count, 0);
    std::uint64_t cur_le = 0;

    while (cur_le < extents_needed) {
        std::uint64_t run_len = UINT64_MAX;
        for (std::uint32_t s = 0; s < stripe_count; s++) {
            if (chunk_idx[s] >= stripe_allocs[s].size()) {
                run_len = 0;
                break;
            }
            std::uint64_t avail = stripe_allocs[s][chunk_idx[s]].count - chunk_off[s];
            if (avail < run_len) run_len = avail;
        }
        if (run_len == 0 || run_len == UINT64_MAX) {
            error = "internal allocation mismatch during striped segment creation";
            return false;
        }
        Segment seg;
        seg.start_extent = cur_le;
        seg.extent_count = run_len * stripe_count;
        seg.type = "striped";
        seg.stripe_count = stripe_count;
        seg.stripe_size_sectors = stripe_size_sectors;
        for (std::uint32_t s = 0; s < stripe_count; s++) {
            std::uint64_t pe = stripe_allocs[s][chunk_idx[s]].start_pe + chunk_off[s];
            seg.stripes.push_back({stripe_allocs[s][chunk_idx[s]].pv_key, pe});
            chunk_off[s] += run_len;
            if (chunk_off[s] == stripe_allocs[s][chunk_idx[s]].count) {
                chunk_idx[s]++;
                chunk_off[s] = 0;
            }
        }
        cur_le += seg.extent_count;
        segs.push_back(std::move(seg));
    }

    out_segments = std::move(segs);
    return true;
}

} // namespace lvmfmt
