#pragma once

#include <random>
#include <vector>

#include "umbra/road_graph.hpp"

namespace umbra::test {

/// Random geometric graph: nodes scattered in a ~2 km box, each connected to
/// a few random nearby nodes with straight-line lengths, so the A* heuristic
/// is admissible. Some edges are one-way.
inline RoadGraph random_road_graph(std::size_t n, unsigned seed, double one_way_ratio = 0.2) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> lat(33.40, 33.42);
  std::uniform_real_distribution<double> lng(-111.95, -111.93);
  std::uniform_real_distribution<double> speed_kmh(20, 100);
  std::uniform_real_distribution<double> coin(0, 1);
  std::uniform_int_distribution<std::size_t> pick(0, n - 1);

  RoadGraphBuilder b;
  std::vector<LatLng> pos;
  for (std::size_t i = 0; i < n; ++i) {
    pos.push_back({lat(rng), lng(rng)});
    b.add_node(static_cast<std::int64_t>(i), pos.back());
  }
  for (std::size_t i = 0; i < n * 3; ++i) {
    const auto u = static_cast<NodeId>(pick(rng));
    const auto v = static_cast<NodeId>(pick(rng));
    if (u == v) continue;
    const double len = haversine_m(pos[u], pos[v]);
    const double dur = len / (speed_kmh(rng) / 3.6);
    b.add_edge(u, v, len, dur);
    if (coin(rng) >= one_way_ratio) b.add_edge(v, u, len, dur);
  }
  return b.build();
}

}  // namespace umbra::test
