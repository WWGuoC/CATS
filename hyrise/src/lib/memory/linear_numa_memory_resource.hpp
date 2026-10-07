#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <sstream>

namespace hyrise {

using NumaNodeID = uint32_t;
using NumaNodeIDs = std::vector<NumaNodeID>;
using InterleavingWeights = std::vector<uint32_t>;
using PageLocations = std::vector<int>;
constexpr uint64_t SIZE_GIB = 1024u * 1024 * 1024;

class LinearNumaMemoryResource : public std::pmr::memory_resource {
 private:
  std::byte* _start_addr;
  std::byte* _alloc_addr;
  uint64_t _buffer_size;
  NumaNodeIDs _node_ids;
  InterleavingWeights _node_weights;
  uint32_t _page_size;
  std::unique_ptr<std::mutex> _mutex;

  // Free-list allocator: track allocated blocks and reuse freed space.
  // When do_deallocate is called, the block is returned to _free_blocks
  // (sorted by address for O(log n) coalescing). do_allocate first searches
  // _free_blocks (first-fit) before bump-allocating from the end.
  std::map<std::byte*, size_t> _free_blocks;            // free regions sorted by address
  std::unordered_map<void*, size_t> _allocated_blocks;  // ptr → allocated size (for deallocate)
  uint64_t _allocated_total = 0;                         // running total of actually-allocated bytes

 public:
  LinearNumaMemoryResource(const uint64_t buffer_size, const NumaNodeIDs node_ids,
    const InterleavingWeights node_weights = {});
  LinearNumaMemoryResource(LinearNumaMemoryResource&& other) noexcept;
  LinearNumaMemoryResource(const LinearNumaMemoryResource&) = delete;
  LinearNumaMemoryResource& operator=(const LinearNumaMemoryResource&) = delete;
  LinearNumaMemoryResource& operator=(LinearNumaMemoryResource&&) = delete;
  ~LinearNumaMemoryResource() override;

  bool address_in_range(const void* addr) const;

  std::byte* data() const;

  const NumaNodeIDs& node_ids() const;

  const InterleavingWeights& node_weights() const;

  static void fill_page_locations_weighted_interleaved(PageLocations& page_locations, uint64_t buffer_size,
      const NumaNodeIDs&node_ids, const InterleavingWeights& node_weights);

  static void place_pages(std::byte* const start_addr, uint64_t buffer_size, const PageLocations& target_page_locations);

  bool is_weighted_interleaved() const;

  std::string description() const;

  // Capacity tracking for DRAM budget management (daemon promotion decisions)
  uint64_t allocated_bytes() const;  // bytes consumed so far by bump allocator
  uint64_t capacity() const;         // total buffer size
  uint64_t free_bytes() const;       // remaining capacity

 private:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override;

  void do_deallocate(void* ptr, std::size_t bytes, std::size_t alignment) override;

  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

  void _bind_memory_round_robin_interleaved();

};

}  // namespace hyrise
