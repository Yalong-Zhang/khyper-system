#include "leiden.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <utility>

// Adapted from the local GDS-2026.10-aligned serial benchmark, src/main.cpp.
// Reference: neusight-exp/case_study/community/neo4j_gds_2026_10_aligned_benchmark.
// Preserve its Java RNG, theta=0.01 and refinement/aggregation semantics.
// Integration fixes: zero-volume graphs, bounded runtime, stable edge ordering,
// one-step root mapping and explicit Q_gamma vs Q_1 scoring on the original graph.
namespace leiden {
namespace {
using Node = std::uint32_t;

struct Graph {
    Node n = 0;
    std::vector<Edge> edges;
    std::vector<std::uint64_t> offsets;
    std::vector<Node> to;
    std::vector<double> weight;
    std::vector<double> degree;
    double volume = 0.0;
};

std::uint64_t pair_key(Node a, Node b) {
    if (a > b)
        std::swap(a, b);
    return (static_cast<std::uint64_t>(a) << 32U) | b;
}

void sort_edges(std::vector<Edge> &edges) {
    std::sort(edges.begin(), edges.end(), [](const Edge &a, const Edge &b) {
        return std::pair{a.source, a.target} < std::pair{b.source, b.target};
    });
}

Graph make_graph(Node count, std::vector<Edge> edges, const std::function<void()> &check_budget) {
    Graph graph;
    graph.n = count;
    graph.edges = std::move(edges);
    graph.offsets.assign(static_cast<std::size_t>(count) + 1, 0);
    for (const auto &edge : graph.edges) {
        check_budget();
        if (edge.source >= count || edge.target >= count || edge.source == edge.target ||
            !std::isfinite(edge.weight) || edge.weight <= 0.0) {
            throw std::invalid_argument(
                "Leiden requires positive-weight ordinary undirected edges");
        }
        ++graph.offsets[edge.source + 1];
        ++graph.offsets[edge.target + 1];
    }
    std::partial_sum(graph.offsets.begin(), graph.offsets.end(), graph.offsets.begin());
    graph.to.resize(graph.offsets.back());
    graph.weight.resize(graph.offsets.back());
    auto cursor = graph.offsets;
    graph.degree.assign(count, 0.0);
    for (const auto &edge : graph.edges) {
        check_budget();
        const auto left = cursor[edge.source]++;
        const auto right = cursor[edge.target]++;
        graph.to[left] = edge.target;
        graph.to[right] = edge.source;
        graph.weight[left] = graph.weight[right] = edge.weight;
        graph.degree[edge.source] += edge.weight;
        graph.degree[edge.target] += edge.weight;
    }
    graph.volume = std::accumulate(graph.degree.begin(), graph.degree.end(), 0.0);
    if (!std::isfinite(graph.volume))
        throw std::overflow_error("Leiden graph volume overflow");
    return graph;
}

void compact_labels(std::vector<Node> &labels) {
    std::vector<std::int64_t> mapping(labels.size(), -1);
    Node next = 0;
    for (auto &label : labels) {
        if (mapping.at(label) < 0)
            mapping[label] = next++;
        label = static_cast<Node>(mapping[label]);
    }
}

std::pair<double, double> score(const Graph &root, const std::vector<Node> &labels,
                                double resolution, const std::function<void()> &check_budget) {
    std::vector<double> volume(root.n, 0.0);
    double internal = 0.0;
    for (Node vertex = 0; vertex < root.n; ++vertex) {
        check_budget();
        volume[labels[vertex]] += root.degree[vertex];
    }
    for (const auto &edge : root.edges) {
        check_budget();
        if (labels[edge.source] == labels[edge.target])
            internal += 2.0 * edge.weight;
    }
    double expected = 0.0;
    for (const auto amount : volume) {
        const auto fraction = amount / root.volume;
        expected += fraction * fraction;
    }
    const auto observed = internal / root.volume;
    return {observed - resolution * expected, observed - expected};
}

// Java Random is used for the GDS Leiden refinement phase.  This reproduces
// its source-level random-number semantics in the serial implementation.
class JavaRandom {
  public:
    explicit JavaRandom(std::uint64_t seed) : seed_((seed ^ multiplier) & mask) {
    }
    double next_double() {
        // Sequence these calls explicitly: C++ does not order operands of '+'.
        const auto high = static_cast<std::uint64_t>(next(26));
        const auto low = next(27);
        return static_cast<double>((high << 27U) + low) / static_cast<double>(1ULL << 53U);
    }

  private:
    static constexpr std::uint64_t multiplier = 0x5DEECE66DULL, addend = 0xBULL,
                                   mask = (1ULL << 48U) - 1U;
    std::uint64_t seed_;
    std::uint32_t next(int bits) {
        seed_ = (seed_ * multiplier + addend) & mask;
        return static_cast<std::uint32_t>(seed_ >> (48 - bits));
    }
};

struct LeidenState {
    Graph graph;
    std::vector<Node> labels;
    std::vector<double> node_volume;
    std::vector<double> community_volume;
};

std::uint64_t leiden_local_move(LeidenState &state, double gamma,
                                const std::function<void()> &check_budget) {
    const auto n = state.graph.n;
    std::deque<Node> queue;
    std::vector<std::uint8_t> queued(n, 1U);
    for (Node u = 0; u < n; ++u)
        queue.push_back(u);
    std::vector<std::uint32_t> seen(n, 0U);
    std::vector<double> weights(n, 0.0);
    std::vector<Node> touched;
    std::uint32_t stamp = 0U;
    std::uint64_t swaps = 0U;
    while (!queue.empty()) {
        check_budget();
        const Node u = queue.front();
        queue.pop_front();
        queued[u] = 0U;
        const Node old = state.labels[u];
        const double node_volume = state.node_volume[u];
        ++stamp;
        if (stamp == 0U) {
            std::fill(seen.begin(), seen.end(), 0U);
            stamp = 1U;
        }
        touched.clear();
        for (auto index = state.graph.offsets[u]; index < state.graph.offsets[u + 1U]; ++index) {
            check_budget();
            const Node candidate = state.labels[state.graph.to[index]];
            if (seen[candidate] != stamp) {
                seen[candidate] = stamp;
                weights[candidate] = 0.0;
                touched.push_back(candidate);
            }
            weights[candidate] += state.graph.weight[index];
        }
        const double current_gain =
            std::max(0.0, seen[old] == stamp ? weights[old] : 0.0) -
            node_volume * (state.community_volume[old] - node_volume) * gamma;
        double best_gain = current_gain;
        Node best = old;
        for (const Node candidate : touched) {
            if (candidate == old)
                continue;
            const double gain =
                weights[candidate] - node_volume * state.community_volume[candidate] * gamma;
            if (gain > best_gain || (gain > 0.0 && gain == best_gain && candidate < best)) {
                best_gain = gain;
                best = candidate;
            }
        }
        if (best != old) {
            state.labels[u] = best;
            state.community_volume[best] += node_volume;
            state.community_volume[old] = std::max(0.0, state.community_volume[old] - node_volume);
            ++swaps;
            for (auto index = state.graph.offsets[u]; index < state.graph.offsets[u + 1U];
                 ++index) {
                check_budget();
                const Node v = state.graph.to[index];
                if (state.labels[v] != best && !queued[v]) {
                    queued[v] = 1U;
                    queue.push_back(v);
                }
            }
        }
    }
    return swaps;
}

struct Refined {
    std::vector<Node> labels;
    std::vector<double> volume;
};

Refined leiden_refine(const LeidenState &state, double gamma, double theta, std::uint64_t seed,
                      const std::function<void()> &check_budget) {
    const Node n = state.graph.n;
    std::vector<Node> refined(n);
    std::iota(refined.begin(), refined.end(), 0U);
    std::vector<double> volume = state.node_volume, within(n, 0.0);
    for (Node u = 0; u < n; ++u)
        for (auto index = state.graph.offsets[u]; index < state.graph.offsets[u + 1U]; ++index)
            if (state.labels[u] == state.labels[state.graph.to[index]])
                within[u] += state.graph.weight[index];
    std::vector<std::uint8_t> singleton(n, 1U);
    std::vector<std::uint32_t> seen(n, 0U);
    std::vector<double> weight_to(n, 0.0);
    std::vector<Node> touched;
    std::uint32_t stamp = 0U;
    JavaRandom random(seed);
    const auto well_connected = [&](Node id) {
        const Node original = state.labels[id];
        return within[id] >= gamma * volume[id] * (state.community_volume[original] - volume[id]);
    };
    for (Node u = 0; u < n; ++u) {
        check_budget();
        if (!singleton[u] || !well_connected(u))
            continue;
        ++stamp;
        if (stamp == 0U) {
            std::fill(seen.begin(), seen.end(), 0U);
            stamp = 1U;
        }
        touched.clear();
        for (auto index = state.graph.offsets[u]; index < state.graph.offsets[u + 1U]; ++index) {
            check_budget();
            const Node v = state.graph.to[index];
            if (state.labels[v] != state.labels[u])
                continue;
            const Node candidate = refined[v];
            if (!well_connected(candidate))
                continue;
            if (seen[candidate] != stamp) {
                seen[candidate] = stamp;
                weight_to[candidate] = 0.0;
                touched.push_back(candidate);
            }
            weight_to[candidate] += state.graph.weight[index];
        }
        if (touched.empty())
            continue;
        const Node old = refined[u];
        const double node_volume = state.node_volume[u];
        double total = 0.0, probability_sum = 0.0, best_gain = 0.0;
        Node best = old;
        std::vector<double> probabilities;
        probabilities.reserve(touched.size());
        for (const Node candidate : touched) {
            const double relationship_weight = weight_to[candidate];
            total += relationship_weight;
            const double gain = relationship_weight - node_volume * volume[candidate] * gamma;
            if (gain > best_gain) {
                best_gain = gain;
                best = candidate;
            }
            const double probability = gain >= 0.0 ? std::exp(gain / theta) : 0.0;
            probabilities.push_back(probability);
            probability_sum += probability;
        }
        Node chosen = old;
        if (!std::isfinite(probability_sum) || probability_sum <= 0.0) {
            if (best_gain > 0.0)
                chosen = best;
        } else {
            double draw = probability_sum * random.next_double(), running = 0.0;
            for (std::size_t i = 0; i < touched.size(); ++i) {
                running += probabilities[i];
                if (draw <= running) {
                    chosen = touched[i];
                    break;
                }
            }
        }
        if (chosen != old) {
            refined[u] = chosen;
            if (singleton[chosen])
                singleton[chosen] = 0U;
            volume[chosen] += node_volume;
            volume[old] -= node_volume;
            within[chosen] += total - weight_to[chosen];
        }
    }
    return {std::move(refined), std::move(volume)};
}

struct LeidenAggregate {
    LeidenState state;
    std::vector<Node> old_to_new;
};

// Mirrors GraphAggregationPhase/RelationshipCreator: only inter-community
// relationships enter the next Leiden graph; internal ones are carried in volumes.
LeidenAggregate aggregate_leiden(const LeidenState &old, const Refined &refined,
                                 const std::function<void()> &check_budget) {
    std::vector<std::int64_t> remap(old.graph.n, -1);
    Node new_n = 0;
    for (const Node label : refined.labels)
        if (remap[label] < 0)
            remap[label] = new_n++;
    std::vector<Node> old_to_new(old.graph.n);
    for (Node u = 0; u < old.graph.n; ++u)
        old_to_new[u] = static_cast<Node>(remap[refined.labels[u]]);
    std::unordered_map<std::uint64_t, double> sums;
    sums.reserve(old.graph.edges.size());
    for (const auto &edge : old.graph.edges) {
        check_budget();
        const Node a = old_to_new[edge.source], b = old_to_new[edge.target];
        if (a != b)
            sums[pair_key(a, b)] += edge.weight;
    }
    std::vector<Edge> edges;
    edges.reserve(sums.size());
    for (const auto &[key, value] : sums)
        edges.push_back({static_cast<Node>(key >> 32U), static_cast<Node>(key), value});
    sort_edges(edges);
    LeidenState state;
    state.graph = make_graph(new_n, std::move(edges), check_budget);
    state.node_volume.assign(new_n, 0.0);
    state.labels.assign(new_n, 0U);
    state.community_volume.assign(new_n, 0.0);
    std::vector<std::int64_t> local_to_seed(old.graph.n, -1);
    for (Node old_refined = 0; old_refined < old.graph.n; ++old_refined) {
        if (remap[old_refined] < 0)
            continue;
        const Node aggregate = static_cast<Node>(remap[old_refined]);
        state.node_volume[aggregate] = refined.volume[old_refined];
        const Node local_community = old.labels[old_refined];
        if (local_to_seed[local_community] < 0)
            local_to_seed[local_community] = aggregate;
        state.labels[aggregate] = static_cast<Node>(local_to_seed[local_community]);
        state.community_volume[state.labels[aggregate]] += state.node_volume[aggregate];
    }
    return {std::move(state), std::move(old_to_new)};
}

} // namespace

Result detect(std::size_t vertex_count, std::vector<Edge> edges, const Parameters &parameters,
              const std::function<void()> &check_budget) {
    if (vertex_count >= std::numeric_limits<Node>::max()) {
        throw std::invalid_argument("Leiden vertex count exceeds uint32 capacity");
    }
    if (!std::isfinite(parameters.gamma) || parameters.gamma <= 0.0) {
        throw std::invalid_argument("Leiden gamma must be finite and positive");
    }
    check_budget();
    sort_edges(edges);
    const auto root = make_graph(static_cast<Node>(vertex_count), std::move(edges), check_budget);
    Result answer;
    answer.communities.resize(vertex_count);
    std::iota(answer.communities.begin(), answer.communities.end(), 0U);
    if (root.volume == 0.0) {
        answer.converged = true;
        answer.stop_reason = "zero_total_weight";
        return answer;
    }

    LeidenState state{root, answer.communities, root.degree, root.degree};
    auto root_to_current = answer.communities;
    // Public gamma is the resolution. Normalize once by 2m for local formulas.
    const double normalized_gamma = parameters.gamma / root.volume;
    double previous_objective = 0.0;
    for (int level = 0; level < 10; ++level) {
        check_budget();
        const auto swaps = leiden_local_move(state, normalized_gamma, check_budget);
        std::vector<Node> labels(vertex_count);
        for (Node vertex = 0; vertex < root.n; ++vertex) {
            check_budget();
            labels[vertex] = state.labels[root_to_current[vertex]];
        }
        compact_labels(labels);
        const auto [objective, standard] = score(root, labels, parameters.gamma, check_budget);
        ++answer.levels;
        // Report the best original-graph Q_gamma reached, never a degraded level.
        if (level > 0 && objective < previous_objective) {
            answer.stop_reason = "objective_decreased";
            break;
        }
        answer.communities = std::move(labels);
        answer.modularity_gamma = objective;
        answer.modularity_standard = standard;
        answer.scores_defined = true;
        if (swaps == 0 || (level > 0 && objective - previous_objective < 1e-4)) {
            answer.converged = true;
            answer.stop_reason = swaps == 0 ? "no_moves" : "tolerance";
            break;
        }
        if (level == 9) {
            answer.stop_reason = "max_levels";
            break;
        }
        const auto refined =
            leiden_refine(state, normalized_gamma, 0.01, parameters.seed, check_budget);
        auto aggregated = aggregate_leiden(state, refined, check_budget);
        for (auto &current : root_to_current) {
            check_budget();
            current = aggregated.old_to_new[current];
        }
        state = std::move(aggregated.state);
        previous_objective = objective;
    }
    check_budget();
    return answer;
}

} // namespace leiden
