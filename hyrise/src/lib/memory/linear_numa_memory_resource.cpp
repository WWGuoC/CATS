#include "linear_numa_memory_resource.hpp"

#include <numa.h>
#include <numaif.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <iostream>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace hyrise {

LinearNumaMemoryResource::LinearNumaMemoryResource(const uint64_t buffer_size, const NumaNodeIDs node_ids,
  const InterleavingWeights node_weights)
    : _buffer_size(buffer_size), _node_ids(node_ids), _node_weights(node_weights),
      _page_size(getpagesize()), _mutex(std::make_unique<std::mutex>()) {
  // Init NUMA
  static auto numa_initialized = false;
  if (!numa_initialized) {
    numa_available();
    numa_set_strict(1);
    numa_initialized = true;
  }
  // MAP_NORESERVE?
  _start_addr =
      static_cast<std::byte*>(mmap(nullptr, _buffer_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));

  if (_start_addr == MAP_FAILED) {
    throw std::runtime_error("mmap failed for buffer size " + std::to_string(_buffer_size) +
                             ": " + std::string(strerror(errno)));
  }

  if (node_weights.empty()) {
    _bind_memory_round_robin_interleaved();
    std::cout << description() << ": Bound region for round robin page interleaving.\n" << std::flush;
  }

  // Prefaulting
  const auto page_count = _buffer_size / _page_size;
  for (auto page_id = uint64_t{0}; page_id < page_count; ++page_id) {
    _start_addr[page_id * _page_size] = std::byte{0};
  }

  if (!_node_weights.empty()) {
    auto page_locations = PageLocations{};
    fill_page_locations_weighted_interleaved(page_locations, _buffer_size, _node_ids, _node_weights);
    place_pages(_start_addr, _buffer_size, page_locations);
    std::cout << description() << ": Moved pages for weighted interleaving.\n" << std::flush;
  }

  _alloc_addr = _start_addr;
}

LinearNumaMemoryResource::LinearNumaMemoryResource(LinearNumaMemoryResource&& other) noexcept :
  _start_addr(other._start_addr), _alloc_addr(other._alloc_addr), _buffer_size(other._buffer_size),
  _node_ids(std::move(other._node_ids)), _node_weights(std::move(other._node_weights)),
  _page_size(other._page_size), _mutex(std::move(other._mutex)),
  _free_blocks(std::move(other._free_blocks)),
  _allocated_blocks(std::move(other._allocated_blocks)),
  _allocated_total(other._allocated_total) {
  // Prevent the moved-from object from munmap'ing the same region.
  other._start_addr = nullptr;
  other._alloc_addr = nullptr;
  other._buffer_size = 0;
  other._allocated_total = 0;
}

LinearNumaMemoryResource::~LinearNumaMemoryResource() {
  if (_start_addr) {
    munmap(_start_addr, _buffer_size);
  }
}

std::byte* LinearNumaMemoryResource::data() const {
  return _start_addr;
}

const NumaNodeIDs& LinearNumaMemoryResource::node_ids() const {
  return _node_ids;
}

std::string LinearNumaMemoryResource::description() const {
  auto sstream = std::stringstream{};
  for (auto& value : node_ids()) {
    if (sstream.str().empty()) {
      sstream << value;
    } else {
      sstream << ", " << value;
    }
  }
  return std::string{"LinearNuma "} + sstream.str();
}

const InterleavingWeights& LinearNumaMemoryResource::node_weights() const {
  return _node_weights;
}

void* LinearNumaMemoryResource::do_allocate(std::size_t bytes, std::size_t alignment) {
  auto lock = std::lock_guard{*_mutex};

  // 1. First-fit search in free blocks (reclaim previously deallocated space)
  for (auto it = _free_blocks.begin(); it != _free_blocks.end(); ++it) {
    auto block_addr = reinterpret_cast<std::uintptr_t>(it->first);
    auto block_size = it->second;

    // Align up within the block
    auto aligned_addr_int = (block_addr + (alignment - 1)) & ~(alignment - 1);
    auto padding = aligned_addr_int - block_addr;
    auto needed = padding + bytes;

    if (needed <= block_size) {
      // Found a fitting free block
      auto aligned_addr = reinterpret_cast<std::byte*>(aligned_addr_int);
      auto block_start = it->first;
      _free_blocks.erase(it);

      // Return leading padding to free list (avoid wasting alignment gap)
      if (padding > 0) {
        _free_blocks[block_start] = padding;
      }
      // Return trailing remainder to free list
      auto remainder = block_size - needed;
      if (remainder > 0) {
        _free_blocks[aligned_addr + bytes] = remainder;
      }

      _allocated_blocks[aligned_addr] = bytes;
      _allocated_total += bytes;
      return aligned_addr;
    }
  }

  // 2. Bump-allocate from the end (no free block fits)
  auto old_alloc_addr = _alloc_addr;
  auto alloc_addr_int = reinterpret_cast<std::uintptr_t>(_alloc_addr);
  auto aligned_addr_int = (alloc_addr_int + (alignment - 1)) & ~(alignment - 1);
  auto aligned_addr = reinterpret_cast<std::byte*>(aligned_addr_int);
  auto padding = static_cast<std::size_t>(aligned_addr_int - alloc_addr_int);

  if ((aligned_addr + bytes) > (_start_addr + _buffer_size)) {
    throw std::logic_error("Not enough buffer capacity");
  }

  // Return leading padding to free list (avoid wasting alignment gap)
  if (padding > 0) {
    _free_blocks[old_alloc_addr] = padding;
  }
  _alloc_addr = aligned_addr + bytes;
  _allocated_blocks[aligned_addr] = bytes;
  _allocated_total += bytes;
  return aligned_addr;
}

void LinearNumaMemoryResource::do_deallocate(void* ptr, std::size_t bytes, std::size_t alignment) {
  auto lock = std::lock_guard{*_mutex};

  // Look up the original allocation size (pmr_vector may pass a different size)
  auto it = _allocated_blocks.find(ptr);
  if (it == _allocated_blocks.end()) {
    return;  // not tracked — nothing to do
  }

  auto block_addr = static_cast<std::byte*>(ptr);
  auto block_size = it->second;
  _allocated_blocks.erase(it);
  _allocated_total -= block_size;

  // Add to free list (sorted by address for coalescing)
  _free_blocks[block_addr] = block_size;

  // Coalesce with adjacent free blocks
  auto current = _free_blocks.find(block_addr);

  // Merge with previous block if adjacent
  if (current != _free_blocks.begin()) {
    auto prev = std::prev(current);
    if (prev->first + prev->second == block_addr) {
      prev->second += block_size;
      _free_blocks.erase(current);
      current = prev;
    }
  }

  // Merge with next block if adjacent
  auto next = std::next(current);
  if (next != _free_blocks.end()) {
    if (current->first + current->second == next->first) {
      current->second += next->second;
      _free_blocks.erase(next);
    }
  }
}

bool LinearNumaMemoryResource::do_is_equal(const std::pmr::memory_resource& other) const noexcept {
  return (this == &other);
}

void LinearNumaMemoryResource::_bind_memory_round_robin_interleaved() {
  if (_node_ids.empty()) {
    return;
  }
  const auto* const allowed_memory_nodes_mask = numa_get_mems_allowed();
  auto* const nodemask = numa_allocate_nodemask();
  if (!nodemask) {
    throw std::logic_error{"numa_allocate_nodemask() returned null"};
  }
  for (const auto node_id : _node_ids) {
    if (!numa_bitmask_isbitset(allowed_memory_nodes_mask, node_id)) {
      throw std::logic_error("Memory allocation on numa node id not allowed (given: " + std::to_string(node_id) + ").");
    }
    numa_bitmask_setbit(nodemask, node_id);
  }

  // Note that "[i]f the MPOL_INTERLEAVE policy was specified, pages already residing on the specified nodes will not be
  // moved such that they are interleaved", see mbind(2) manual.
  const auto mbind_succeeded = mbind(_start_addr, _buffer_size, MPOL_INTERLEAVE, nodemask->maskp, nodemask->size + 1,
                                     MPOL_MF_STRICT | MPOL_MF_MOVE) == 0;
  const auto mbind_errno = errno;
  if (!mbind_succeeded) {
    auto ss = std::stringstream{};
    ss << "mbind failed: " << strerror(mbind_errno)
       << ". You might have run out of space on the node(s) or reached the maximum map "
       << "limit (vm.max_map_count).";
    throw std::logic_error(ss.str());
  }
  numa_bitmask_free(nodemask);
}

void LinearNumaMemoryResource::fill_page_locations_weighted_interleaved(PageLocations& page_locations,
  uint64_t buffer_size, const NumaNodeIDs& node_ids, const InterleavingWeights& node_weights) {
  const auto page_size = getpagesize();
  const auto page_count = buffer_size / page_size;
  auto cur_node_position = 0u;
  auto cur_node_page_count = 0u;
  const auto node_count = node_ids.size();

  page_locations.resize(page_count);
  for (auto page_idx = 0u; page_idx < page_count; ++page_idx) {
    if (cur_node_page_count == node_weights[cur_node_position]) {
      cur_node_position = (cur_node_position + 1) % node_count;
      cur_node_page_count = 0;
    }
    page_locations[page_idx] = node_ids[cur_node_position];
    ++cur_node_page_count;
  }
}

void LinearNumaMemoryResource::place_pages(std::byte* const start_addr, uint64_t buffer_size,
  const PageLocations& target_page_locations) {
  if (target_page_locations.empty()) {
    throw std::logic_error("Page placement failed: no target page locations are given.");
  }

  const auto page_size = getpagesize();
  const auto page_count = buffer_size / page_size;

  // move_pages requires a vector of void*, one void* per page.
  auto pages = std::vector<void*>{};
  pages.resize(page_count);

  for (auto page_idx = uint64_t{0}; page_idx < page_count; ++page_idx) {
    pages[page_idx] = reinterpret_cast<void*>(start_addr + (page_idx * page_size));
  }

  // move_pages, initialize with value that is not a valid NUMA node idx. We assume that max int is not a valid idx.
  auto page_status = std::vector<int>(page_count, std::numeric_limits<int>::max());

  const auto ret =
      move_pages(0, page_count, pages.data(), target_page_locations.data(), page_status.data(), MPOL_MF_MOVE);
  const auto move_pages_errno = errno;

  if (ret != 0) {
    throw std::logic_error(std::string{"move_pages failed: "} + strerror(move_pages_errno));
  }
}

bool LinearNumaMemoryResource::address_in_range(const void* addr) const {
  return _start_addr <= addr && addr < _start_addr + _buffer_size;
}

bool LinearNumaMemoryResource::is_weighted_interleaved() const {
  return !_node_weights.empty() && _node_weights.size() == _node_ids.size();
}

uint64_t LinearNumaMemoryResource::allocated_bytes() const {
  auto lock = std::lock_guard{*_mutex};
  return _allocated_total;
}

uint64_t LinearNumaMemoryResource::capacity() const {
  return _buffer_size;
}

uint64_t LinearNumaMemoryResource::free_bytes() const {
  auto lock = std::lock_guard{*_mutex};
  return _buffer_size - _allocated_total;
}

}  // namespace hyrise
