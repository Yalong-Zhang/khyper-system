#pragma once

#include <optional>

#include "khypergraph/types.hpp"

namespace khypergraph {

enum class VertexKind {
    Atom,
    Entity,
    Community,
};

struct Vertex {
    VertexId id;
    LabelId label;
    VertexKind kind{VertexKind::Atom};
    PropertyMap properties;

    // A vertex in layer k can represent an edge in layer k-1. Input-layer
    // vertices have no source edge and keep their data locally.
    std::optional<EdgeRef> lifted_from;
};

}  // namespace khypergraph
