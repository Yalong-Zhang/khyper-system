#pragma once

#include <compare>
#include <cstdint>
#include <span>
#include <vector>

#include "khypergraph/types.hpp"

namespace khypergraph {

enum class EdgeKind {
    Relation,
    Cluster,
    DerivedRelation,
};

// The role of a vertex in a hyperedge. A hyperedge is either undirected
// (every endpoint is Member) or directed (one or more Tail endpoints point
// to one or more Head endpoints).
enum class EndpointRole : std::uint8_t {
    Member,
    Tail,
    Head,
};

struct EdgeEndpoint {
    VertexId vertex;
    EndpointRole role{EndpointRole::Member};

    constexpr EdgeEndpoint() = default;
    constexpr EdgeEndpoint(
        VertexId vertex_id,
        EndpointRole endpoint_role = EndpointRole::Member) noexcept
        : vertex(vertex_id), role(endpoint_role) {}

    constexpr bool operator==(const EdgeEndpoint&) const = default;
    constexpr auto operator<=>(const EdgeEndpoint&) const = default;
    constexpr bool operator==(VertexId other) const noexcept {
        return vertex == other;
    }

    friend constexpr bool operator==(
        VertexId lhs,
        const EdgeEndpoint& rhs) noexcept {
        return lhs == rhs.vertex;
    }
};

[[nodiscard]] inline std::vector<EdgeEndpoint> member_endpoints(
    std::span<const VertexId> vertices) {
    std::vector<EdgeEndpoint> result;
    result.reserve(vertices.size());
    for (const VertexId vertex : vertices) {
        result.emplace_back(vertex, EndpointRole::Member);
    }
    return result;
}

struct Edge {
    EdgeId id;
    LabelId label;
    EdgeKind kind{EdgeKind::Relation};

    // HyperGraph normalizes endpoints into (vertex, role) order and removes
    // duplicate pins. The same vertex may be both Tail and Head.
    std::vector<EdgeEndpoint> endpoints;

    PropertyMap properties;

    // Lower-layer edges that caused this edge to be created. An empty list on
    // a non-input layer means that the edge is synthetic at that layer.
    std::vector<EdgeRef> derived_from;

    [[nodiscard]] bool is_directed() const noexcept {
        for (const EdgeEndpoint& endpoint : endpoints) {
            if (endpoint.role != EndpointRole::Member) {
                return true;
            }
        }
        return false;
    }
};

}  // namespace khypergraph
