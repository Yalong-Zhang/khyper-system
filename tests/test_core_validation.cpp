#include "khypergraph/hyper_graph.hpp"
#include <stdexcept>
#include <type_traits>

using namespace khypergraph;
static_assert(std::is_nothrow_move_constructible_v<PropertyMap>);
static_assert(std::is_nothrow_move_constructible_v<Edge>);
void require(bool ok) {
    if (!ok) throw std::runtime_error("incidence validation regression");
}

int main() {
    // Typed properties survive compact storage, copies, updates and erasure.
    PropertyMap properties{{"weight", std::int64_t{7}}, {"name", std::string{"author \"FX\""}}};
    properties.reserve(8);
    require(!properties.emplace("weight", std::int64_t{99}).second);
    require(std::get<std::int64_t>(properties.at("weight")) == 7);
    auto copied = properties;
    copied.insert_or_assign("weight", std::int64_t{9});
    copied["nullable"] = std::monostate{};
    require(properties.size() == 2 && copied.size() == 3);
    require(std::get<std::int64_t>(properties.at("weight")) == 7);
    require(copied.erase("name") == 1 && copied.erase("missing") == 0);
    copied.shrink_to_fit();
    const auto moved = std::move(copied);
    require(moved.contains("nullable") && !moved.contains("name"));
    require(std::get<std::int64_t>(moved.at("weight")) == 9);
    require(std::get<std::string>(properties.at("name")) == "author \"FX\"");
    bool missing = false;
    try { (void)moved.at("missing"); } catch (const std::out_of_range&) { missing = true; }
    require(missing);

    HyperGraph graph;
    require(graph.validate());
    const auto a = graph.add_vertex(Vertex{});
    const auto b = graph.add_vertex(Vertex{});
    const auto isolated = graph.add_vertex(Vertex{});
    graph.set_vertex_property(a, "name", std::string{"original"});
    graph.set_vertex_property(a, "name", std::string{"updated"});
    require(std::get<std::string>(graph.vertex(a).properties.at("name")) == "updated");
    Edge edge;
    edge.endpoints = {{a, EndpointRole::Tail}, {a, EndpointRole::Head},
                      {b, EndpointRole::Head}};
    const auto first = graph.add_edge(edge);
    const auto second = graph.add_edge(edge);
    graph.set_edge_property(first, "weight", std::int64_t{3});
    graph.set_edge_property(second, "weight", std::int64_t{4});
    require(std::get<std::int64_t>(graph.edge(first).properties.at("weight")) == 3);
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
