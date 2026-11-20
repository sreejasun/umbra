// Benchmark: Dijkstra vs A* on random origin/destination pairs of a real road network.
//
// Usage: umbra_bench [map.osm.json] [queries=500] [seed=42]
//
// For every query both algorithms must return the same optimal cost; the tool
// exits non-zero if they ever disagree.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "umbra/osm_loader.hpp"
#include "umbra/shortest_path.hpp"

namespace {

struct Samples {
  std::vector<double> millis;
  std::vector<double> settled;
};

double mean(const std::vector<double>& v) {
  double sum = 0;
  for (double x : v) sum += x;
  return v.empty() ? 0 : sum / static_cast<double>(v.size());
}

double percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const auto idx = static_cast<std::size_t>(std::ceil(p * static_cast<double>(v.size()))) - 1;
  return v[std::min(idx, v.size() - 1)];
}

}  // namespace

int main(int argc, char** argv) {
  const std::string path = argc > 1 ? argv[1] : UMBRA_DEFAULT_DATA;
  const int queries = argc > 2 ? std::atoi(argv[2]) : 500;
  const unsigned seed = argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 42U;

  umbra::RoadGraph graph;
  try {
    graph = umbra::load_overpass_file(path);
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  std::cout << "Graph: " << graph.node_count() << " nodes, " << graph.edge_count() << " edges\n"
            << "Queries: " << queries << " random pairs (seed " << seed << ")\n\n";

  std::mt19937 rng(seed);
  std::uniform_int_distribution<umbra::NodeId> pick(0, static_cast<umbra::NodeId>(graph.node_count() - 1));
  std::vector<std::pair<umbra::NodeId, umbra::NodeId>> pairs(static_cast<std::size_t>(queries));
  for (auto& p : pairs) p = {pick(rng), pick(rng)};

  int mismatches = 0;
  std::cout << std::left << std::setw(10) << "metric" << std::setw(10) << "algorithm" << std::right
            << std::setw(14) << "avg settled" << std::setw(12) << "avg ms" << std::setw(12) << "p50 ms"
            << std::setw(12) << "p95 ms" << "\n"
            << std::string(70, '-') << "\n";

  for (umbra::Metric metric : {umbra::Metric::kTime, umbra::Metric::kDistance}) {
    Samples dijkstra;
    Samples astar;
    for (const auto& [s, t] : pairs) {
      const auto d = umbra::find_shortest_path(graph, s, t, {metric, umbra::Algorithm::kDijkstra});
      const auto a = umbra::find_shortest_path(graph, s, t, {metric, umbra::Algorithm::kAStar});
      if (d.found != a.found || std::abs(d.cost - a.cost) > 1e-6 * std::max(1.0, d.cost)) ++mismatches;
      dijkstra.millis.push_back(static_cast<double>(d.elapsed.count()) / 1000.0);
      dijkstra.settled.push_back(static_cast<double>(d.settled_count));
      astar.millis.push_back(static_cast<double>(a.elapsed.count()) / 1000.0);
      astar.settled.push_back(static_cast<double>(a.settled_count));
    }

    const char* metric_name = metric == umbra::Metric::kTime ? "time" : "distance";
    for (const auto& [name, samples] : {std::pair{"dijkstra", &dijkstra}, std::pair{"astar", &astar}}) {
      std::cout << std::left << std::setw(10) << metric_name << std::setw(10) << name << std::right
                << std::fixed << std::setprecision(0) << std::setw(14) << mean(samples->settled)
                << std::setprecision(3) << std::setw(12) << mean(samples->millis) << std::setw(12)
                << percentile(samples->millis, 0.50) << std::setw(12)
                << percentile(samples->millis, 0.95) << "\n";
    }
    std::cout << "  A* settled " << std::setprecision(1)
              << 100.0 * (1.0 - mean(astar.settled) / mean(dijkstra.settled))
              << "% fewer nodes than Dijkstra\n\n";
  }

  if (mismatches > 0) {
    std::cerr << "FAIL: " << mismatches << " queries where Dijkstra and A* disagree\n";
    return 1;
  }
  std::cout << "OK: Dijkstra and A* returned identical optimal costs for all queries.\n";
  return 0;
}
