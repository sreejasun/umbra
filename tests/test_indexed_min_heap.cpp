#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <random>
#include <stdexcept>

#include "umbra/indexed_min_heap.hpp"

namespace umbra {
namespace {

TEST(IndexedMinHeap, StartsEmpty) {
  IndexedMinHeap<double> heap(10);
  EXPECT_TRUE(heap.empty());
  EXPECT_EQ(heap.size(), 0U);
  EXPECT_EQ(heap.capacity(), 10U);
  EXPECT_FALSE(heap.contains(3));
}

TEST(IndexedMinHeap, PopsInPriorityOrder) {
  IndexedMinHeap<double> heap(5);
  heap.push(0, 5.0);
  heap.push(1, 1.0);
  heap.push(2, 3.0);
  heap.push(3, 4.0);
  heap.push(4, 2.0);

  std::vector<std::uint32_t> order;
  while (!heap.empty()) order.push_back(heap.pop().first);
  EXPECT_EQ(order, (std::vector<std::uint32_t>{1, 4, 2, 3, 0}));
}

TEST(IndexedMinHeap, TiesBreakByIdForDeterminism) {
  IndexedMinHeap<double> heap(4);
  heap.push(3, 1.0);
  heap.push(1, 1.0);
  heap.push(2, 1.0);
  EXPECT_EQ(heap.pop().first, 1U);
  EXPECT_EQ(heap.pop().first, 2U);
  EXPECT_EQ(heap.pop().first, 3U);
}

TEST(IndexedMinHeap, DecreaseKeyMovesEntryToTop) {
  IndexedMinHeap<double> heap(3);
  heap.push(0, 10.0);
  heap.push(1, 20.0);
  heap.push(2, 30.0);
  heap.decrease_key(2, 5.0);
  EXPECT_EQ(heap.top().first, 2U);
  EXPECT_DOUBLE_EQ(heap.priority(2), 5.0);
  EXPECT_EQ(heap.size(), 3U);  // no duplicates, unlike lazy deletion
}

TEST(IndexedMinHeap, PushOrDecreaseOnlyLowers) {
  IndexedMinHeap<double> heap(2);
  EXPECT_TRUE(heap.push_or_decrease(0, 10.0));
  EXPECT_FALSE(heap.push_or_decrease(0, 15.0));
  EXPECT_DOUBLE_EQ(heap.priority(0), 10.0);
  EXPECT_TRUE(heap.push_or_decrease(0, 7.0));
  EXPECT_DOUBLE_EQ(heap.priority(0), 7.0);
}

TEST(IndexedMinHeap, ContainsTracksMembership) {
  IndexedMinHeap<double> heap(3);
  heap.push(1, 1.0);
  EXPECT_TRUE(heap.contains(1));
  heap.pop();
  EXPECT_FALSE(heap.contains(1));
  heap.push(1, 2.0);  // can re-insert after removal
  EXPECT_TRUE(heap.contains(1));
}

TEST(IndexedMinHeap, RejectsInvalidOperations) {
  IndexedMinHeap<double> heap(2);
  EXPECT_THROW(heap.pop(), std::out_of_range);
  EXPECT_THROW((void)heap.top(), std::out_of_range);
  EXPECT_THROW(heap.push(2, 1.0), std::out_of_range);
  heap.push(0, 1.0);
  EXPECT_THROW(heap.push(0, 1.0), std::logic_error);
  EXPECT_THROW(heap.decrease_key(0, 2.0), std::logic_error);
  EXPECT_THROW(heap.decrease_key(1, 0.5), std::out_of_range);
  EXPECT_FALSE(heap.contains(99));
}

// Property test: random pushes / decrease-keys / pops agree with a reference model.
TEST(IndexedMinHeap, MatchesReferenceModelUnderRandomOperations) {
  constexpr std::uint32_t kCapacity = 300;
  std::mt19937 rng(1234);
  std::uniform_int_distribution<std::uint32_t> pick_id(0, kCapacity - 1);
  std::uniform_real_distribution<double> pick_priority(0, 1000);
  std::uniform_int_distribution<int> pick_op(0, 2);

  IndexedMinHeap<double> heap(kCapacity);
  std::map<std::uint32_t, double> model;

  for (int step = 0; step < 20'000; ++step) {
    const int op = pick_op(rng);
    if (op == 0) {
      const auto id = pick_id(rng);
      const double p = pick_priority(rng);
      if (model.count(id)) {
        heap.push_or_decrease(id, p);
        model[id] = std::min(model[id], p);
      } else {
        heap.push(id, p);
        model[id] = p;
      }
    } else if (op == 1 && !model.empty()) {
      auto expected = std::min_element(model.begin(), model.end(), [](const auto& a, const auto& b) {
        return a.second < b.second || (a.second == b.second && a.first < b.first);
      });
      const auto [id, priority] = heap.pop();
      ASSERT_EQ(id, expected->first);
      ASSERT_DOUBLE_EQ(priority, expected->second);
      model.erase(expected);
    }
    ASSERT_EQ(heap.size(), model.size());
  }
}

}  // namespace
}  // namespace umbra
