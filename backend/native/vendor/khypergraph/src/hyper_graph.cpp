#include "khypergraph/hyper_graph.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace khypergraph {

namespace {

void validate_endpoint_roles(const Edge& edge) {
    bool has_member = false;
    bool has_tail = false;
    bool has_head = false;

    for (const EdgeEndpoint& endpoint : edge.endpoints) {
        switch (endpoint.role) {
            case EndpointRole::Member:
                has_member = true;
                break;
            case EndpointRole::Tail:
                has_tail = true;
                break;
            case EndpointRole::Head:
                has_head = true;
                break;
        }
    }

    if (has_member && (has_tail || has_head)) {
        throw std::invalid_argument(
            "a hyperedge cannot mix undirected and directed endpoints");
    }
    if (!has_member && (!has_tail || !has_head)) {
        throw std::invalid_argument(
            "a directed hyperedge requires at least one Tail and one Head");
    }
    if (edge.kind == EdgeKind::Cluster && !has_member) {
        throw std::invalid_argument(
            "a cluster hyperedge must be undirected");
    }
}

std::vector<VertexId> unique_endpoint_vertices(const Edge& edge) {
    std::vector<VertexId> result;
    result.reserve(edge.endpoints.size());
    for (const EdgeEndpoint& endpoint : edge.endpoints) {
        if (result.empty() || result.back() != endpoint.vertex) {
            result.push_back(endpoint.vertex);
        }
    }
    return result;
}

}  // namespace

HyperGraph::HyperGraph(
    Level level,
    std::shared_ptr<const HyperGraph> lower)
    : level_(level), lower_(std::move(lower)) {
    if (lower_ != nullptr && lower_->level() + 1 != level_) {
        throw std::invalid_argument(
            "lower graph must be the immediately preceding level");
    }
}

Level HyperGraph::level() const noexcept {
    return level_;
}

const HyperGraph* HyperGraph::lower_graph() const noexcept {
    return lower_.get();
}

std::shared_ptr<const HyperGraph> HyperGraph::lower_graph_ptr() const noexcept {
    return lower_;
}

const HyperGraph& HyperGraph::layer(Level target) const {
    const HyperGraph* current = this;
    while (current != nullptr && current->level() > target) {
        current = current->lower_graph();
    }

    if (current == nullptr || current->level() != target) {
        throw std::out_of_range("graph layer is not reachable");
    }
    return *current;
}

VertexId HyperGraph::add_vertex(Vertex vertex) {
    if (lower_ == nullptr) {
        if (vertex.lifted_from.has_value()) {
            throw std::invalid_argument(
                "an input-layer vertex cannot reference a lower edge");
        }
    } else {
        if (!vertex.lifted_from.has_value()) {
            throw std::invalid_argument(
                "a non-input-layer vertex must reference a lower edge");
        }

        const EdgeRef source = *vertex.lifted_from;
        if (source.level != lower_->level() || !lower_->contains(source.id)) {
            throw std::invalid_argument(
                "lifted vertex references an invalid lower-layer edge");
        }
    }

    const VertexId id{vertices_.size()};
    vertex.id = id;

    vertices_.push_back(std::move(vertex));
    try {
        incidence_.emplace_back();
    } catch (...) {
        vertices_.pop_back();
        throw;
    }

    return id;
}

EdgeId HyperGraph::add_edge(Edge edge) {
    if (edge.endpoints.empty()) {
        throw std::invalid_argument("a hyperedge must have at least one endpoint");
    }

    std::sort(edge.endpoints.begin(), edge.endpoints.end());
    edge.endpoints.erase(
        std::unique(edge.endpoints.begin(), edge.endpoints.end()),
        edge.endpoints.end());

    validate_endpoint_roles(edge);

    for (const EdgeEndpoint& endpoint : edge.endpoints) {
        if (!contains(endpoint.vertex)) {
            throw std::invalid_argument("edge endpoint does not exist in graph");
        }
    }

    for (const EdgeRef source : edge.derived_from) {
        if (lower_ == nullptr || source.level != lower_->level() ||
            !lower_->contains(source.id)) {
            throw std::invalid_argument(
                "derived edge references an invalid lower-layer edge");
        }
    }

    std::sort(edge.derived_from.begin(), edge.derived_from.end());
    edge.derived_from.erase(
        std::unique(edge.derived_from.begin(), edge.derived_from.end()),
        edge.derived_from.end());

    const EdgeId id{edges_.size()};
    edge.id = id;
    const std::vector<VertexId> endpoint_vertices =
        unique_endpoint_vertices(edge);

    edges_.push_back(std::move(edge));
    try {
        for (const VertexId endpoint : endpoint_vertices) {
            incidence_[endpoint.value].push_back(id);
        }
    } catch (...) {
        for (const VertexId endpoint : endpoint_vertices) {
            auto& incident = incidence_[endpoint.value];
            if (!incident.empty() && incident.back() == id) {
                incident.pop_back();
            }
        }
        edges_.pop_back();
        throw;
    }

    return id;
}

void HyperGraph::reserve_vertices(std::size_t capacity) {
    vertices_.reserve(capacity);
    incidence_.reserve(capacity);
}

void HyperGraph::reserve_edges(std::size_t capacity) {
    edges_.reserve(capacity);
}

void HyperGraph::reserve_incidence(
    std::span<const std::size_t> capacities) {
    if (capacities.size() != incidence_.size()) {
        throw std::invalid_argument(
            "incidence capacity count must equal vertex count");
    }

    for (std::size_t vertex = 0;
         vertex < capacities.size();
         ++vertex) {
        incidence_[vertex].reserve(capacities[vertex]);
    }
}

bool HyperGraph::contains(VertexId id) const noexcept {
    return id.value < vertices_.size();
}

bool HyperGraph::contains(EdgeId id) const noexcept {
    return id.value < edges_.size();
}

const Vertex& HyperGraph::vertex(VertexId id) const {
    if (!contains(id)) {
        throw std::out_of_range("VertexId not found");
    }
    return vertices_[id.value];
}

const Edge& HyperGraph::edge(EdgeId id) const {
    if (!contains(id)) {
        throw std::out_of_range("EdgeId not found");
    }
    return edges_[id.value];
}

void HyperGraph::set_vertex_property(
    VertexId id,
    std::string key,
    PropertyValue value) {
    if (!contains(id)) {
        throw std::out_of_range("VertexId not found");
    }
    if (key.empty()) {
        throw std::invalid_argument(
            "vertex property key cannot be empty");
    }

    vertices_[id.value].properties.insert_or_assign(
        std::move(key),
        std::move(value));
}

void HyperGraph::set_edge_property(
    EdgeId id,
    std::string key,
    PropertyValue value) {
    if (!contains(id)) {
        throw std::out_of_range("EdgeId not found");
    }
    if (key.empty()) {
        throw std::invalid_argument(
            "edge property key cannot be empty");
    }

    edges_[id.value].properties.insert_or_assign(
        std::move(key),
        std::move(value));
}

std::span<const Vertex> HyperGraph::vertices() const noexcept {
    return vertices_;
}

std::span<const Edge> HyperGraph::edges() const noexcept {
    return edges_;
}

std::span<const EdgeId> HyperGraph::incident_edges(VertexId id) const {
    if (!contains(id)) {
        throw std::out_of_range("VertexId not found");
    }
    return incidence_[id.value];
}

const HyperGraph& HyperGraph::base_graph() const noexcept {
    const HyperGraph* current = this;
    while (current->lower_ != nullptr) {
        current = current->lower_.get();
    }
    return *current;
}

void HyperGraph::collect_base_vertices(
    VertexId id,
    std::vector<VertexId>& output) const {
    const Vertex& current = vertex(id);

    if (lower_ == nullptr) {
        output.push_back(current.id);
        return;
    }

    if (!current.lifted_from.has_value()) {
        throw std::logic_error(
            "non-input vertex has no lower-layer source edge");
    }

    const EdgeRef source = *current.lifted_from;
    if (source.level != lower_->level()) {
        throw std::logic_error(
            "vertex source edge does not belong to the direct lower layer");
    }

    const Edge& source_edge = lower_->edge(source.id);
    for (const EdgeEndpoint& endpoint : source_edge.endpoints) {
        lower_->collect_base_vertices(endpoint.vertex, output);
    }
}

std::vector<VertexId> HyperGraph::base_vertices(VertexId id) const {
    std::vector<VertexId> result;
    collect_base_vertices(id, result);

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<EdgeRef> HyperGraph::base_connections(
    VertexId lhs,
    VertexId rhs) const {
    const std::vector<VertexId> left = base_vertices(lhs);
    const std::vector<VertexId> right = base_vertices(rhs);
    const std::unordered_set<VertexId, VertexIdHash> right_set(
        right.begin(), right.end());

    const HyperGraph& input = base_graph();
    std::unordered_set<EdgeId, EdgeIdHash> found;

    for (const VertexId left_vertex : left) {
        for (const EdgeId edge_id : input.incident_edges(left_vertex)) {
            const Edge& candidate = input.edge(edge_id);
            if (candidate.kind == EdgeKind::Cluster) {
                continue;
            }

            const bool intersects_right = std::any_of(
                candidate.endpoints.begin(),
                candidate.endpoints.end(),
                [&right_set](const EdgeEndpoint& endpoint) {
                    return right_set.contains(endpoint.vertex);
                });

            if (intersects_right) {
                found.insert(edge_id);
            }
        }
    }

    std::vector<EdgeRef> result;
    result.reserve(found.size());
    for (const EdgeId edge_id : found) {
        result.push_back(EdgeRef{input.level(), edge_id});
    }
    std::sort(result.begin(), result.end());
    return result;
}

bool HyperGraph::validate() const {
    if (lower_ != nullptr && lower_->level() + 1 != level_) {
        return false;
    }
    if (lower_ != nullptr && !lower_->validate()) {
        return false;
    }

    if (vertices_.size() != incidence_.size()) {
        return false;
    }

    for (std::size_t i = 0; i < vertices_.size(); ++i) {
        const Vertex& current = vertices_[i];
        if (current.id.value != i) {
            return false;
        }

        if (lower_ == nullptr) {
            if (current.lifted_from.has_value()) {
                return false;
            }
        } else {
            if (!current.lifted_from.has_value()) {
                return false;
            }
            const EdgeRef source = *current.lifted_from;
            if (source.level != lower_->level() || !lower_->contains(source.id)) {
                return false;
            }
        }
    }

    // add_edge appends incident edge IDs in increasing order. Check the same
    // invariant with one cursor per vertex instead of rebuilding hash sets.
    std::vector<std::size_t> incidence_cursor(vertices_.size(), 0);

    for (std::size_t i = 0; i < edges_.size(); ++i) {
        const Edge& current = edges_[i];
        if (current.id.value != i || current.endpoints.empty() ||
            !std::is_sorted(current.endpoints.begin(), current.endpoints.end()) ||
            std::adjacent_find(current.endpoints.begin(), current.endpoints.end()) !=
                current.endpoints.end()) {
            return false;
        }

        bool has_member = false;
        bool has_tail = false;
        bool has_head = false;
        std::optional<VertexId> previous_vertex;
        for (const EdgeEndpoint& endpoint : current.endpoints) {
            if (!contains(endpoint.vertex)) {
                return false;
            }
            // A directed hyperedge can contain the same vertex in both roles;
            // incidence contains that edge once, as in unique_endpoint_vertices.
            if (!previous_vertex || *previous_vertex != endpoint.vertex) {
                auto& cursor = incidence_cursor[endpoint.vertex.value];
                const auto& incident = incidence_[endpoint.vertex.value];
                if (cursor >= incident.size() || incident[cursor] != current.id) {
                    return false;
                }
                ++cursor;
                previous_vertex = endpoint.vertex;
            }

            switch (endpoint.role) {
                case EndpointRole::Member:
                    has_member = true;
                    break;
                case EndpointRole::Tail:
                    has_tail = true;
                    break;
                case EndpointRole::Head:
                    has_head = true;
                    break;
            }
        }
        if ((has_member && (has_tail || has_head)) ||
            (!has_member && (!has_tail || !has_head)) ||
            (current.kind == EdgeKind::Cluster && !has_member)) {
            return false;
        }

        if (!std::is_sorted(
                current.derived_from.begin(), current.derived_from.end()) ||
            std::adjacent_find(
                current.derived_from.begin(), current.derived_from.end()) !=
                current.derived_from.end()) {
            return false;
        }

        for (const EdgeRef source : current.derived_from) {
            if (lower_ == nullptr || source.level != lower_->level() ||
                !lower_->contains(source.id)) {
                return false;
            }
        }
    }

    for (std::size_t i = 0; i < incidence_.size(); ++i) {
        if (incidence_cursor[i] != incidence_[i].size()) {
            return false;
        }
    }

    return true;
}

}  // namespace khypergraph
