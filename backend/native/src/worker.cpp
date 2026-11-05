#include "khypergraph/hyper_graph.hpp"
#include "dataset_reader.hpp"
#include "query_engine.hpp"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

using json = nlohmann::json;
using namespace khypergraph;
using I = std::int64_t;
using Neighbors = std::vector<std::pair<std::size_t, I>>;

I checked_add(I a, I b) {
    if (b > 0 && a > std::numeric_limits<I>::max() - b) throw std::runtime_error("weight overflow");
    return a + b;
}
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

class OrdinaryGraph {
    HyperGraph graph_{1};
    std::vector<I> ids_, inflow_, outflow_;
    std::vector<std::string> names_, folded_;
    std::unordered_map<I, std::size_t> id_to_index_;
    json metadata_;
    bool directed_;
    std::vector<Neighbors> in_, out_;
    std::vector<std::size_t> unique_;
    std::size_t raw_edges_{0}, self_edges_{0};
    I total_weight_{0};
    double load_seconds_{0};

    static void compact(Neighbors& list) {
        std::sort(list.begin(), list.end());
        std::size_t n = 0;
        for (const auto entry : list) {
            if (n && list[n-1].first == entry.first) throw std::runtime_error("duplicate logical edge");
            else list[n++] = entry;
        }
        list.resize(n);
    }
    json vertex(std::size_t i) const { return {{"vertex_id", std::to_string(ids_[i])}, {"name", names_[i]}}; }
    std::size_t index(const json& p) const {
        if (!p.contains("vertex_id") || !p["vertex_id"].is_string()) throw std::invalid_argument("vertex_id must be a decimal string");
        const auto text=p["vertex_id"].get<std::string>();
        if(text.empty() || (text.size()>1 && text[0]=='0') || text.find_first_not_of("0123456789")!=std::string::npos)throw std::invalid_argument("invalid vertex_id");
        const auto found = id_to_index_.find(std::stoll(text));
        if (found == id_to_index_.end()) throw std::invalid_argument("vertex not found; search_vertices first");
        return found->second;
    }
    static int limit(const json& p) {
        const int n = p.value("limit", 10);
        if (n < 1 || n > 50) throw std::invalid_argument("limit must be between 1 and 50");
        return n;
    }
    I metric(std::size_t i, bool weighted, const std::string& m) const {
        if (!directed_) {
            if(weighted && m=="strength") return outflow_[i];
            if(!weighted && m=="degree") return static_cast<I>(out_[i].size());
            throw std::invalid_argument("undirected graphs support strength or degree, not directional metrics");
        }
        if (weighted) {
            if (m == "inflow") return inflow_[i];
            if (m == "outflow") return outflow_[i];
            if (m == "total") return checked_add(inflow_[i],outflow_[i]);
            if (m == "net_inflow") return inflow_[i]-outflow_[i];
        } else {
            if (m == "in_degree") return static_cast<I>(in_[i].size());
            if (m == "out_degree") return static_cast<I>(out_[i].size());
            if (m == "total_degree") return static_cast<I>(in_[i].size()+out_[i].size());
            if (m == "unique_partners") return static_cast<I>(unique_[i]);
        }
        throw std::invalid_argument("unsupported ranking metric for this weighted/unweighted tool");
    }
public:
    explicit OrdinaryGraph(const std::filesystem::path& descriptor_path) {
        const auto started = std::chrono::steady_clock::now();
        const auto d=dataset::read_descriptor(descriptor_path);
        dataset::Table vertices(d.at("vertices"),descriptor_path.parent_path(),"vertices");
        dataset::Table edges(d.at("edges"),descriptor_path.parent_path(),"edges");
        metadata_=dataset::metadata(d);directed_=metadata_.at("directed").get<bool>();
        metadata_["vertex_fields"]=vertices.schema();metadata_["edge_fields"]=edges.schema();
        metadata_["capabilities"]={"field_schema","field_profiles","attribute_filters","group_aggregation","full_graph_vertex_metrics","neighbor_queries"};
        const auto progress=[](const std::string& phase,std::uint64_t records) {
            std::cout<<json({{"type","progress"},{"phase",phase},{"records",records}}).dump()<<std::endl;
        };
        const auto nv=vertices.count_hint(),ne=edges.count_hint();
        graph_.reserve_vertices(nv);graph_.reserve_edges(ne);
        ids_.reserve(nv);names_.reserve(nv);folded_.reserve(nv);id_to_index_.reserve(nv);
        vertices.read([&](dataset::Record&& record) {
            const auto i=ids_.size();const I rid=std::get<I>(record.at("id"));
            if(!id_to_index_.emplace(rid,i).second)throw std::runtime_error("duplicate external vertex ID "+std::to_string(rid));
            auto found=record.find("name");
            const std::string name=found!=record.end() && std::holds_alternative<std::string>(found->second)?std::get<std::string>(found->second):std::to_string(rid);
            ids_.push_back(rid);names_.push_back(name);folded_.push_back(lower(name));
            Vertex v;v.kind=VertexKind::Entity;v.properties=std::move(record);
            graph_.add_vertex(std::move(v));
        },progress);
        const auto count=ids_.size();
        in_.resize(count);out_.resize(count);unique_.resize(count);inflow_.resize(count);outflow_.resize(count);
        edges.read([&](dataset::Record&& record) {
            const I src=std::get<I>(record.at("source")),dst=std::get<I>(record.at("target")),w=std::get<I>(record.at("weight"));
            ++raw_edges_;
            const auto source=id_to_index_.find(src),target=id_to_index_.find(dst);
            if(source==id_to_index_.end() || target==id_to_index_.end())throw std::runtime_error("edge references missing vertex: "+std::to_string(src)+" -> "+std::to_string(dst));
            if(src==dst)throw std::runtime_error("self-loop at external vertex "+std::to_string(src));
            const auto u=source->second,v=target->second;
            Edge edge;edge.kind=EdgeKind::Relation;
            edge.endpoints={{VertexId{u},directed_?EndpointRole::Tail:EndpointRole::Member},{VertexId{v},directed_?EndpointRole::Head:EndpointRole::Member}};
            record.erase("source");record.erase("target");edge.properties=std::move(record);
            graph_.add_edge(std::move(edge));
            out_[u].emplace_back(v,w);in_[v].emplace_back(u,w);
            inflow_[v]=checked_add(inflow_[v],w);outflow_[u]=checked_add(outflow_[u],w);
            if(!directed_){out_[v].emplace_back(u,w);in_[u].emplace_back(v,w);inflow_[u]=checked_add(inflow_[u],w);outflow_[v]=checked_add(outflow_[v],w);}
            total_weight_=checked_add(total_weight_,w);
        },progress);
        progress("validating",0);
        for (std::size_t i=0;i<count;++i) {
            compact(in_[i]); compact(out_[i]);
            std::size_t a=0,b=0,partners=0;
            while(a<in_[i].size() || b<out_[i].size()) {
                ++partners;
                if (b==out_[i].size() || (a<in_[i].size() && in_[i][a].first<out_[i][b].first)) ++a;
                else if(a==in_[i].size() || out_[i][b].first<in_[i][a].first) ++b;
                else {++a;++b;}
            }
            unique_[i]=partners;
        }
        if (!graph_.validate()) throw std::runtime_error("KHyperGraph invariant validation failed");
        vertices.check_unchanged();edges.check_unchanged();
        load_seconds_=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
    }
    json info() const {
        auto result=metadata_;
        result["dataset"]=metadata_.at("name"); result["vertices"]=ids_.size();result["edges"]=graph_.edges().size();
        result["total_weight"]=total_weight_;result["load_seconds"]=load_seconds_;result["storage"]="KHyperGraph / level 1";
        return result;
    }
    json call(const std::string& op,const json& p) const {
        if(op=="get_dataset_info") return info();
        const QueryEngine queries(graph_,ids_,metadata_,directed_);
        if(QueryEngine::handles(op))return queries.call(op,p);
        if(op=="search_vertices") {
            const auto query=lower(p.at("query").get<std::string>());
            if(query.empty() || query.size()>200) throw std::invalid_argument("query must contain 1–200 characters");
            std::vector<std::pair<int,std::size_t>> hits;
            for(std::size_t i=0;i<ids_.size();++i) {
                if(std::to_string(ids_[i])==query || folded_[i]==query) hits.emplace_back(0,i);
                else if(folded_[i].find(query)!=std::string::npos) hits.emplace_back(2,i);
            }
            std::sort(hits.begin(),hits.end(),[&](auto a,auto b){return a.first!=b.first?a.first<b.first:ids_[a.second]<ids_[b.second];});
            json rows=json::array();
            for(std::size_t k=0;k<std::min(hits.size(),static_cast<std::size_t>(limit(p)));++k) {
                auto row=vertex(hits[k].second);row["exact_match"]=hits[k].first==0;
                row["match_type"]=hits[k].first==0?"exact":hits[k].first==1?"configured_alias":"substring";

                const auto& props=graph_.vertex(VertexId{hits[k].second}).properties;
                json properties=json::object();
                for(const auto& [key,value]:props)if(key!="id" && key!="name")std::visit([&](const auto& v){using T=std::decay_t<decltype(v)>;if constexpr(std::is_same_v<T,std::monostate>)properties[key]=nullptr;else properties[key]=v;},value);
                row["properties"]=properties;
                rows.push_back(row);
            }
            return {{"rows",rows},{"total_matches",hits.size()},{"truncated",rows.size()<hits.size()}};
        }
        const bool weighted=op.ends_with("_weighted");
        if(op=="get_vertex_stats_weighted" || op=="get_vertex_stats_unweighted") {
            const auto i=index(p); auto result=vertex(i);
            if(!directed_) {if(weighted)result["strength"]=outflow_[i];else result["degree"]=out_[i].size();}
            else if(weighted) {result["inflow"]=inflow_[i];result["outflow"]=outflow_[i];result["total"]=checked_add(inflow_[i],outflow_[i]);result["net_inflow"]=inflow_[i]-outflow_[i];}
            else {result["in_degree"]=in_[i].size();result["out_degree"]=out_[i].size();result["total_degree"]=in_[i].size()+out_[i].size();result["unique_partners"]=unique_[i];}
            result["weighted"]=weighted;return result;
        }
        if(op=="rank_vertices_weighted" || op=="rank_vertices_unweighted") {
            const auto m=p.value("metric",directed_?(weighted?"total":"total_degree"):(weighted?"strength":"degree"));
            // Validate even an empty graph.
            const std::vector<std::string> allowed=weighted?std::vector<std::string>{"inflow","outflow","total","net_inflow"}:std::vector<std::string>{"in_degree","out_degree","total_degree","unique_partners"};
            if((directed_ && std::find(allowed.begin(),allowed.end(),m)==allowed.end()) || (!directed_ && m!=(weighted?"strength":"degree")))throw std::invalid_argument("invalid metric");
            const auto accept=queries.vertex_filter(p.value("filter",json::array()));
            std::vector<std::size_t> order;for(std::size_t i=0;i<ids_.size();++i)if(accept(i))order.push_back(i);
            const auto count=std::min(order.size(),static_cast<std::size_t>(limit(p)));
            std::partial_sort(order.begin(),order.begin()+count,order.end(),[&](auto a,auto b){const auto va=metric(a,weighted,m),vb=metric(b,weighted,m);return va!=vb?va>vb:ids_[a]<ids_[b];});
            json rows=json::array();
            for(std::size_t k=0;k<count;++k){auto row=vertex(order[k]);row["rank"]=k+1;row["value"]=metric(order[k],weighted,m);rows.push_back(row);}
            return {{"rows",rows},{"metric",m},{"weighted",weighted},{"total_vertices",ids_.size()},{"matched_vertices",order.size()},{"truncated",count<order.size()},{"sort","descending, then vertex_id ascending"},{"scope","full_graph_metrics; filter restricts candidates only"}};
        }
        if(op=="get_vertex_neighbors_weighted" || op=="get_vertex_neighbors_unweighted") {
            const auto i=index(p);const auto direction=p.value("direction",directed_?"out":"both");
            if((directed_ && direction!="in" && direction!="out") || (!directed_ && direction!="both"))throw std::invalid_argument("direction must be in or out");
            auto neighbors=direction=="in"?in_[i]:out_[i];
            const auto total_neighbors=neighbors.size();
            const auto accept=queries.vertex_filter(p.value("neighbor_filter",json::array()));
            std::erase_if(neighbors,[&](const auto& entry){return !accept(entry.first);});
            I matched_weight=0;if(weighted)for(const auto& entry:neighbors)matched_weight=checked_add(matched_weight,entry.second);
            std::sort(neighbors.begin(),neighbors.end(),[&](auto a,auto b){return weighted && a.second!=b.second?a.second>b.second:ids_[a.first]<ids_[b.first];});
            const I total=direction=="in"?inflow_[i]:outflow_[i];
            json rows=json::array();
            for(std::size_t k=0;k<std::min(neighbors.size(),static_cast<std::size_t>(limit(p)));++k) {
                auto row=vertex(neighbors[k].first);
                if(weighted){row["weight"]=neighbors[k].second;row["share"]=total>0?json(static_cast<double>(neighbors[k].second)/static_cast<double>(total)):json(nullptr);}
                rows.push_back(row);
            }
            json result={{"vertex",vertex(i)},{"direction",direction},{"weighted",weighted},{"rows",rows},{"total_neighbors",neighbors.size()},{"truncated",rows.size()<neighbors.size()},
                         {"sort",weighted?"weight descending, then vertex_id ascending":"vertex_id ascending (not a strength ranking)"}};
            result["total_neighbors"]=total_neighbors;result["matched_neighbors"]=neighbors.size();
            result["scope"]="full_graph_neighbors; neighbor_filter restricts returned candidates only";
            if(weighted){result["total_weight"]=total;result["matched_weight"]=matched_weight;result["share_denominator"]="full_direction_total_weight";}
            return result;
        }
        throw std::invalid_argument("unknown tool");
    }
};

int main(int argc,char** argv) {
    try {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
#endif
        // Only small JSON control messages cross the process boundary.
        std::ios::sync_with_stdio(false);
        std::cin.tie(nullptr);
        (void)argc;(void)argv;
        std::string header;if(!std::getline(std::cin,header))throw std::runtime_error("missing load request");
        const auto load_request=json::parse(header);
        const auto path_text=load_request.at("descriptor_path").get<std::string>();
        OrdinaryGraph graph(std::filesystem::path(std::u8string(path_text.begin(),path_text.end())));
        std::cout<<json({{"type","ready"},{"data",graph.info()}}).dump()<<std::endl;
        std::string line;
        while(std::getline(std::cin,line)) {
            json id=nullptr;
            try {
                const auto request=json::parse(line); id=request.value("id",json());
                const auto result=graph.call(request.at("tool").get<std::string>(),request.value("arguments",json::object()));
                std::cout<<json({{"id",id},{"ok",true},{"data",result}}).dump()<<std::endl;
            }catch(const std::exception& e){std::cout<<json({{"id",id},{"ok",false},{"error",e.what()}}).dump()<<std::endl;}
        }
    }catch(const std::exception& e){std::cerr<<"worker failed: "<<e.what()<<std::endl;return 1;}
}
