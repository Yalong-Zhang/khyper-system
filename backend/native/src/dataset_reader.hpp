#pragma once
#include "khypergraph/types.hpp"
#include "nlohmann/json.hpp"
#include <filesystem>
#include <functional>
#include <memory>

namespace dataset {
using Record = khypergraph::PropertyMap;
using Progress = std::function<void(const std::string&, std::uint64_t)>;

// Each reader streams bounded Arrow batches or one TSV record at a time.
class Table {
public:
    Table(const nlohmann::json& spec, const std::filesystem::path& base,
          const std::string& kind);
    ~Table();
    std::uint64_t count_hint() const;
    nlohmann::json schema() const;
    void read(const std::function<void(Record&&)>& consume, const Progress& progress);
    void check_unchanged() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

nlohmann::json read_descriptor(const std::filesystem::path& path);
nlohmann::json metadata(const nlohmann::json& descriptor);
}
