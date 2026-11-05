#include "khypergraph/hyper_graph.hpp"
#include <stdexcept>

using namespace khypergraph;
void require(bool ok) {
    if (!ok) throw std::runtime_error("incidence validation regression");
}

int main() {
    HyperGraph graph;
    require(graph.validate());
    const auto a = graph.add_vertex(Vertex{});
    const auto b = graph.add_vertex(Vertex{});
    const auto isolated = graph.add_vertex(Vertex{});
    Edge edge;
    edge.endpoints = {{a, EndpointRole::Tail}, {a, EndpointRole::Head},
                      {b, EndpointRole::Head}};
    const auto first = graph.add_edge(edge);
    const auto second = graph.add_edge(edge);
    require(graph.validate());
    require(graph.incident_edges(a).size() == 2);
    require(graph.incident_edges(isolated).empty());

    // Deliberately corrupt the non-const graph through its read-only view:
    // duplicate/missing/out-of-range incident IDs must all be detected.
    auto* incidence = const_cast<EdgeId*>(graph.incident_edges(a).data());
    incidence[1] = first;
    require(!graph.validate());
    incidence[1] = EdgeId{999};
    require(!graph.validate());
    incidence[1] = second;
    require(graph.validate());

    // Changing an edge's endpoint leaves a stale index at the old vertex.
    auto& changed = const_cast<Edge&>(graph.edge(second));
    changed.endpoints.back().vertex = isolated;
    require(!graph.validate());
    changed.endpoints.back().vertex = b;
    require(graph.validate());
    return 0;
}
