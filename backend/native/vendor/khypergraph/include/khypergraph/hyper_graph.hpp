#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "khypergraph/edge.hpp"
#include "khypergraph/vertex.hpp"

namespace khypergraph {

class HyperGraph {
public:
    explicit HyperGraph(
        Level level = 0,
        std::shared_ptr<const HyperGraph> lower = nullptr);

    [[nodiscard]] Level level() const noexcept;
    [[nodiscard]] const HyperGraph* lower_graph() const noexcept;
    [[nodiscard]] std::shared_ptr<const HyperGraph> lower_graph_ptr() const noexcept;
    [[nodiscard]] const HyperGraph& layer(Level target) const;

    // Ids are assigned densely from 0 in insertion order and equal the
    // element's position in vertices_/edges_. Any preset id is overwritten.
    VertexId add_vertex(Vertex vertex);
    EdgeId add_edge(Edge edge);

    // Capacity hints for bulk loaders. They never change graph contents.
    void reserve_vertices(std::size_t capacity);
    void reserve_edges(std::size_t capacity);
    void reserve_incidence(
        std::span<const std::size_t> capacities);

    [[nodiscard]] bool contains(VertexId id) const noexcept;
    [[nodiscard]] bool contains(EdgeId id) const noexcept;

    [[nodiscard]] const Vertex& vertex(VertexId id) const;
    [[nodiscard]] const Edge& edge(EdgeId id) const;

    // Mutates vertex metadata without exposing the graph's structural fields.
    // Existing values with the same key are overwritten.
    void set_vertex_property(
        VertexId id,
        std::string key,
        PropertyValue value);
    void set_edge_property(
        EdgeId id,
        std::string key,
        PropertyValue value);

    [[nodiscard]] std::span<const Vertex> vertices() const noexcept;
    [[nodiscard]] std::span<const Edge> edges() const noexcept;
    [[nodiscard]] std::span<const EdgeId> incident_edges(VertexId id) const;

    // Recursively expands a vertex to the vertices stored in the lowest
    // reachable input layer. The result is sorted and duplicate-free.
    [[nodiscard]] std::vector<VertexId> base_vertices(VertexId id) const;

    // Returns relation edges in the input layer that intersect both expanded
    // vertex sets. Cluster edges are not treated as original relationships.
    [[nodiscard]] std::vector<EdgeRef> base_connections(
        VertexId lhs,
        VertexId rhs) const;

    // Checks ids, lineage references, endpoints, and the incidence index.
    [[nodiscard]] bool validate() const;

private:
    [[nodiscard]] const HyperGraph& base_graph() const noexcept;
    void collect_base_vertices(
        VertexId id,
        std::vector<VertexId>& output) const;

    Level level_;
    std::shared_ptr<const HyperGraph> lower_;

    std::vector<Vertex> vertices_;
    std::vector<Edge> edges_;

    // Indexed by the corresponding position in vertices_.
    std::vector<std::vector<EdgeId>> incidence_;
};

}  // namespace khypergraph
