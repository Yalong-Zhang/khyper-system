#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <variant>

namespace khypergraph {

struct VertexId {
    std::uint64_t value{0};

    constexpr auto operator<=>(const VertexId&) const = default;
};

struct EdgeId {
    std::uint64_t value{0};

    constexpr auto operator<=>(const EdgeId&) const = default;
};

struct LabelId {
    std::uint32_t value{0};

    constexpr auto operator<=>(const LabelId&) const = default;
};

using Level = std::uint32_t;

// A stable reference to an edge in a specific graph layer. Edge ids are
// dense and local to a layer, so the level is part of the reference.
struct EdgeRef {
    Level level{0};
    EdgeId id;

    constexpr auto operator<=>(const EdgeRef&) const = default;
};

using PropertyValue = std::variant<
    std::monostate,
    bool,
    std::int64_t,
    double,
    std::string>;

using PropertyMap = std::unordered_map<std::string, PropertyValue>;

struct VertexIdHash {
    std::size_t operator()(VertexId id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value);
    }
};

struct EdgeIdHash {
    std::size_t operator()(EdgeId id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value);
    }
};

}  // namespace khypergraph
