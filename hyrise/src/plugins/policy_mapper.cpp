#include "policy_mapper.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace hyrise {

void PolicyMapper::map(std::vector<SegmentPlacementDecision>& decisions) {
  if (decisions.empty()) return;

  uint64_t hot_threshold = 0;
  uint64_t cold_threshold = 0;
  uint64_t large_volume_threshold = 0;
  compute_thresholds(decisions, hot_threshold, cold_threshold, large_volume_threshold);

  std::cout << "- [Δ1] Stage 4 (Policy Mapper): hot_threshold=" << hot_threshold
            << ", cold_threshold=" << cold_threshold
            << ", large_volume_threshold=" << large_volume_threshold << "\n";

  auto local_count = size_t{0};
  auto cxl_count = size_t{0};

  for (auto& decision : decisions) {
    // Decision rules
    switch (decision.access_type) {
      case AccessType::Scan:
        if (decision.access_frequency > hot_threshold) {
          decision.target_tier = Tier::Local;
        } else if (decision.access_frequency <= cold_threshold) {
          decision.target_tier = Tier::CXL;
        } else {
          decision.target_tier = Tier::Local;  // medium frequency → default local
        }
        break;

      case AccessType::PointLookup:
        if (decision.access_frequency > hot_threshold) {
          decision.target_tier = Tier::Local;
        } else if (decision.access_frequency <= cold_threshold) {
          decision.target_tier = Tier::CXL;
        } else {
          decision.target_tier = Tier::Local;
        }
        break;

      case AccessType::HashBuild:
        if (decision.data_volume > large_volume_threshold && decision.is_immutable) {
          decision.target_tier = Tier::CXL;  // large immutable hash-build → CXL
        } else if (decision.access_frequency > hot_threshold) {
          decision.target_tier = Tier::Local;
        } else {
          decision.target_tier = Tier::CXL;
        }
        break;

      case AccessType::HashProbe:
        if (decision.access_frequency > hot_threshold) {
          decision.target_tier = Tier::Local;
        } else {
          decision.target_tier = Tier::CXL;
        }
        break;

      case AccessType::Aggregate:
        // CPU-bound: always keep in local DRAM for fast access
        decision.target_tier = Tier::Local;
        break;

      case AccessType::Sort:
        if (decision.access_frequency > hot_threshold) {
          decision.target_tier = Tier::Local;
        } else {
          decision.target_tier = Tier::CXL;
        }
        break;

      case AccessType::Unknown:
        // No LQP info (e.g., Δ1 ablation): use frequency only
        if (decision.access_frequency > hot_threshold) {
          decision.target_tier = Tier::Local;
        } else if (decision.access_frequency <= cold_threshold) {
          decision.target_tier = Tier::CXL;
        } else {
          decision.target_tier = Tier::Local;
        }
        break;
    }

    // Additional rule: immutable + cold → CXL (zero consistency cost)
    if (decision.is_immutable && decision.access_frequency <= cold_threshold) {
      decision.target_tier = Tier::CXL;
    }

    // Count for logging
    switch (decision.target_tier) {
      case Tier::Local: ++local_count; break;
      case Tier::CXL: ++cxl_count; break;
      default: break;
    }
  }

  std::cout << "- [Δ1] Stage 4: " << local_count << " Local, " << cxl_count << " CXL\n";
}

void PolicyMapper::map_dram_first(std::vector<SegmentPlacementDecision>& decisions, float demote_pct,
                                  uint64_t dram_budget_bytes) {
  if (decisions.empty()) return;

  // Default: everything stays on Local (DRAM).
  for (auto& decision : decisions) {
    decision.target_tier = Tier::Local;
    decision.create_replica = false;
  }

  // Sort a copy of indices by a "coldness" score: lowest frequency first,
  // and among equal frequencies, immutable segments first (they are safe to
  // demote — zero consistency cost). Mutable segments are excluded from demotion.
  auto indices = std::vector<size_t>{};
  indices.reserve(decisions.size());
  for (auto i = size_t{0}; i < decisions.size(); ++i) {
    if (!decisions[i].is_immutable) continue;  // mutable → keep on DRAM
    indices.push_back(i);
  }

  std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
    if (decisions[a].access_frequency != decisions[b].access_frequency)
      return decisions[a].access_frequency < decisions[b].access_frequency;
    // Among equal frequencies, prefer demoting immutable segments
    return decisions[a].is_immutable && !decisions[b].is_immutable;
  });

  // Demote the coldest demote_pct fraction to CXL
  const auto demote_count = static_cast<size_t>(std::ceil(indices.size() * demote_pct));
  auto local_count = size_t{0};
  auto cxl_count = size_t{0};
  auto local_bytes = uint64_t{0};
  for (const auto& decision : decisions) {
    if (decision.target_tier == Tier::Local) local_bytes += decision.data_volume;
  }
  for (auto i = size_t{0}; i < demote_count && i < indices.size(); ++i) {
    decisions[indices[i]].target_tier = Tier::CXL;
    local_bytes -= decisions[indices[i]].data_volume;
    ++cxl_count;
  }
  // Exact byte-budget clamp (repair G1/G2): keep demoting in the same
  // coldness order until the Local byte sum fits the budget.
  auto clamped = size_t{0};
  if (dram_budget_bytes > 0) {
    for (auto i = demote_count; i < indices.size() && local_bytes > dram_budget_bytes; ++i) {
      decisions[indices[i]].target_tier = Tier::CXL;
      local_bytes -= decisions[indices[i]].data_volume;
      ++cxl_count;
      ++clamped;
    }
  }
  local_count = decisions.size() - cxl_count;

  std::cout << "- [DRAM-first] Pre-placement: " << local_count << " stay Local (DRAM), "
            << cxl_count << " demoted to CXL (" << demote_pct * 100 << "% of "
            << indices.size() << " immutable segments)";
  if (dram_budget_bytes > 0) {
    std::cout << "; byte-budget clamp demoted " << clamped
              << " more (Local bytes " << local_bytes << " <= budget " << dram_budget_bytes
              << ")";
  }
  std::cout << "\n";
}

void PolicyMapper::compute_thresholds(const std::vector<SegmentPlacementDecision>& decisions,
                                       uint64_t& hot_threshold, uint64_t& cold_threshold,
                                       uint64_t& large_volume_threshold) {
  // Collect all frequencies
  auto frequencies = std::vector<uint64_t>{};
  auto volumes = std::vector<uint64_t>{};
  frequencies.reserve(decisions.size());
  volumes.reserve(decisions.size());

  for (const auto& d : decisions) {
    frequencies.push_back(d.access_frequency);
    volumes.push_back(d.data_volume);
  }

  // Sort to compute percentile thresholds
  std::sort(frequencies.begin(), frequencies.end());
  std::sort(volumes.begin(), volumes.end());

  const auto n = frequencies.size();
  if (n == 0) return;

  // hot = 70th percentile, cold = 30th percentile
  hot_threshold = frequencies[static_cast<size_t>(n * 0.7)];
  cold_threshold = frequencies[static_cast<size_t>(n * 0.3)];
  // large volume = median
  large_volume_threshold = volumes[n / 2];
}

}  // namespace hyrise
