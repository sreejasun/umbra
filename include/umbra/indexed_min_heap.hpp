#pragma once

#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace umbra {

/**
 * Indexed binary min-heap: a priority queue over integer ids in [0, capacity)
 * that supports decrease-key.
 *
 * A plain binary heap cannot find an element to lower its priority, so the
 * usual Dijkstra workaround is to push duplicates and skip stale entries
 * ("lazy deletion"), which lets the heap grow to O(E). This heap keeps a
 * position table `pos_[id]` that is updated on every swap, so decrease-key is
 * an O(log n) sift-up and the heap never holds more than one entry per node.
 *
 * Layout: entry i has children 2i+1 and 2i+2 and parent (i-1)/2.
 *
 *   push / pop / decrease_key : O(log n)
 *   top / contains / priority : O(1)
 *   memory                    : O(capacity)
 *
 * Ties are broken by id so results are deterministic.
 */
template <typename Priority = double>
class IndexedMinHeap {
 public:
  using Id = std::uint32_t;

  explicit IndexedMinHeap(std::size_t capacity) : pos_(capacity, kAbsent) {
    heap_.reserve(capacity < 1024 ? capacity : 1024);
  }

  [[nodiscard]] bool empty() const noexcept { return heap_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return heap_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return pos_.size(); }

  [[nodiscard]] bool contains(Id id) const noexcept {
    return id < pos_.size() && pos_[id] != kAbsent;
  }

  [[nodiscard]] Priority priority(Id id) const {
    if (!contains(id)) throw std::out_of_range("IndexedMinHeap::priority: id not in heap");
    return heap_[pos_[id]].priority;
  }

  /// Smallest (id, priority) pair without removing it.
  [[nodiscard]] std::pair<Id, Priority> top() const {
    if (heap_.empty()) throw std::out_of_range("IndexedMinHeap::top: heap is empty");
    return {heap_.front().id, heap_.front().priority};
  }

  /// Insert a new id. Throws if the id is out of range or already present.
  void push(Id id, Priority priority) {
    if (id >= pos_.size()) throw std::out_of_range("IndexedMinHeap::push: id out of range");
    if (pos_[id] != kAbsent) throw std::logic_error("IndexedMinHeap::push: id already in heap");
    heap_.push_back({priority, id});
    pos_[id] = static_cast<std::uint32_t>(heap_.size() - 1);
    sift_up(heap_.size() - 1);
  }

  /// Lower the priority of an id already in the heap.
  void decrease_key(Id id, Priority priority) {
    if (!contains(id)) throw std::out_of_range("IndexedMinHeap::decrease_key: id not in heap");
    std::size_t i = pos_[id];
    if (heap_[i].priority < priority) {
      throw std::logic_error("IndexedMinHeap::decrease_key: new priority is larger");
    }
    heap_[i].priority = priority;
    sift_up(i);
  }

  /// Insert, or lower the priority if already present and the new one is smaller.
  /// Returns true if the heap changed.
  bool push_or_decrease(Id id, Priority priority) {
    if (!contains(id)) {
      push(id, priority);
      return true;
    }
    if (priority < heap_[pos_[id]].priority) {
      decrease_key(id, priority);
      return true;
    }
    return false;
  }

  /// Remove and return the smallest (id, priority) pair.
  std::pair<Id, Priority> pop() {
    if (heap_.empty()) throw std::out_of_range("IndexedMinHeap::pop: heap is empty");
    Entry root = heap_.front();
    move_entry(heap_.size() - 1, 0);
    heap_.pop_back();
    pos_[root.id] = kAbsent;
    if (!heap_.empty()) sift_down(0);
    return {root.id, root.priority};
  }

 private:
  struct Entry {
    Priority priority;
    Id id;
  };

  static constexpr std::uint32_t kAbsent = std::numeric_limits<std::uint32_t>::max();

  [[nodiscard]] static bool less(const Entry& a, const Entry& b) noexcept {
    return a.priority < b.priority || (!(b.priority < a.priority) && a.id < b.id);
  }

  void move_entry(std::size_t from, std::size_t to) noexcept {
    heap_[to] = heap_[from];
    pos_[heap_[to].id] = static_cast<std::uint32_t>(to);
  }

  void sift_up(std::size_t i) noexcept {
    const Entry entry = heap_[i];
    while (i > 0) {
      const std::size_t parent = (i - 1) / 2;
      if (!less(entry, heap_[parent])) break;
      move_entry(parent, i);
      i = parent;
    }
    heap_[i] = entry;
    pos_[entry.id] = static_cast<std::uint32_t>(i);
  }

  void sift_down(std::size_t i) noexcept {
    const std::size_t n = heap_.size();
    const Entry entry = heap_[i];
    while (true) {
      std::size_t child = 2 * i + 1;
      if (child >= n) break;
      if (child + 1 < n && less(heap_[child + 1], heap_[child])) ++child;
      if (!less(heap_[child], entry)) break;
      move_entry(child, i);
      i = child;
    }
    heap_[i] = entry;
    pos_[entry.id] = static_cast<std::uint32_t>(i);
  }

  std::vector<Entry> heap_;
  std::vector<std::uint32_t> pos_;  // id -> index in heap_, or kAbsent
};

}  // namespace umbra
