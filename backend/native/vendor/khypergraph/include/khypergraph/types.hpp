#pragma once

#include <compare>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

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

// Graph records have few properties. Contiguous pairs avoid a bucket array and
// separately allocated hash nodes for every edge, while retaining typed values.
// This project's map operations intentionally match the former unordered map.
class PropertyMap {
    using Storage = std::vector<std::pair<std::string, PropertyValue>>;
    Storage values_;

public:
    using value_type = Storage::value_type;
    using iterator = Storage::iterator;
    using const_iterator = Storage::const_iterator;

    PropertyMap() = default;
    PropertyMap(std::initializer_list<value_type> values) {
        reserve(values.size());
        for (const auto& item : values) emplace(item.first, item.second);
    }

    iterator begin() noexcept { return values_.begin(); }
    iterator end() noexcept { return values_.end(); }
    const_iterator begin() const noexcept { return values_.begin(); }
    const_iterator end() const noexcept { return values_.end(); }
    std::size_t size() const noexcept { return values_.size(); }
    bool empty() const noexcept { return values_.empty(); }
    void reserve(std::size_t size) { values_.reserve(size); }
    void shrink_to_fit() { values_.shrink_to_fit(); }
    void clear() noexcept { values_.clear(); }

    iterator find(std::string_view key) {
        return std::find_if(begin(), end(), [key](const auto& item) { return item.first == key; });
    }
    const_iterator find(std::string_view key) const {
        return std::find_if(begin(), end(), [key](const auto& item) { return item.first == key; });
    }
    bool contains(std::string_view key) const { return find(key) != end(); }
    PropertyValue& at(std::string_view key) {
        const auto found = find(key);
        if (found == end()) throw std::out_of_range("property not found");
        return found->second;
    }
    const PropertyValue& at(std::string_view key) const {
        const auto found = find(key);
        if (found == end()) throw std::out_of_range("property not found");
        return found->second;
    }
    PropertyValue& operator[](std::string key) {
        return emplace(std::move(key), PropertyValue{}).first->second;
    }
    std::pair<iterator, bool> emplace(std::string key, PropertyValue value) {
        const auto found = find(key);
        if (found != end()) return {found, false};
        values_.emplace_back(std::move(key), std::move(value));
        return {std::prev(end()), true};
    }
    std::pair<iterator, bool> insert_or_assign(std::string key, PropertyValue value) {
        const auto found = find(key);
        if (found == end()) return emplace(std::move(key), std::move(value));
        found->second = std::move(value);
        return {found, false};
    }
    std::size_t erase(std::string_view key) {
        const auto found = find(key);
        if (found == end()) return 0;
        values_.erase(found);
        return 1;
    }
};

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
