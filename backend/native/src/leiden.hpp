#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace leiden {

struct Edge {
    std::uint32_t source;
    std::uint32_t target;
    double weight;
};

struct Parameters {
    double gamma = 1.0;
    std::uint64_t seed = 20261029;
};

struct Result {
    std::vector<std::uint32_t> communities;
    double modularity_gamma = 0.0;
    double modularity_standard = 0.0;
    bool scores_defined = false;
    bool converged = false;
    int levels = 0;
    std::string stop_reason;
};

// Edges are unique positive-weight unordered pairs. Isolated vertices are retained.
// check_budget is also called inside local move, refinement and aggregation loops.
Result detect(std::size_t vertex_count, std::vector<Edge> edges, const Parameters &parameters,
              const std::function<void()> &check_budget);

} // namespace leiden
