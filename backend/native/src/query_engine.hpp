#pragma once
#include "khypergraph/hyper_graph.hpp"
#include "nlohmann/json.hpp"
#include <functional>

// Read-only typed queries. Filtering never constructs a new graph or alters metrics.
class QueryEngine {
    using json=nlohmann::json;
    using Value=khypergraph::PropertyValue;
    const khypergraph::HyperGraph& graph_;
    const std::vector<std::int64_t>& ids_;
    const json& metadata_;
    bool directed_;
    enum class Scope {Vertex,Edge,Source,Target};
    struct Field {Scope scope;std::string name,type;bool id;};
    Field resolve(const std::string& name,bool edges) const;
    Value value(std::size_t row,const Field& field) const;
    std::pair<std::size_t,std::size_t> endpoints(std::size_t edge) const;
    static Value literal(const json& input,const Field& field);
    static json encode(const Value& value,bool id=false);
    static int compare(const Value& a,const Value& b);
    using Predicate=std::function<bool(std::size_t)>;
    Predicate filter(const json& conditions,bool edges) const;
    Predicate selection(const json& parameters,bool edges) const;
    json schema(const json& parameters) const;
    json profile(const json& parameters) const;
    json query(const json& parameters,bool edges) const;
    json aggregate(const json& parameters,bool edges) const;
public:
    QueryEngine(const khypergraph::HyperGraph& graph,const std::vector<std::int64_t>& ids,const json& metadata,bool directed)
        :graph_(graph),ids_(ids),metadata_(metadata),directed_(directed){}
    static bool handles(const std::string& op);
    json call(const std::string& op,const json& parameters) const;
    Predicate vertex_filter(const json& conditions) const {return filter(conditions,false);}
};
