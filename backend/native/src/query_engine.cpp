#include "query_engine.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

using json=nlohmann::json;
using Value=khypergraph::PropertyValue;
using I=std::int64_t;
static void expect(bool condition,const std::string& error) {if(!condition)throw std::invalid_argument(error);}
static bool null(const Value& value){return std::holds_alternative<std::monostate>(value);}
static I add(I a,I b) {
    if((b>0 && a>std::numeric_limits<I>::max()-b) || (b<0 && a<std::numeric_limits<I>::min()-b))throw std::overflow_error("int64 aggregate overflow");
    return a+b;
}
static int row_limit(const json& p){const int n=p.value("limit",10);expect(n>=1 && n<=50,"limit must be 1..50");return n;}
static std::string entity(const json& p){const auto name=p.at("entity").get<std::string>();expect(name=="vertices" || name=="edges","invalid entity");return name;}
static std::string query_name(const std::string& name,bool edges) {
    return edges && (name.starts_with("source.") || name.starts_with("target.") || name.starts_with("edge."))?"edge."+name:name;
}

QueryEngine::Field QueryEngine::resolve(const std::string& path,bool edges) const {
    auto scope=edges?Scope::Edge:Scope::Vertex;auto name=path;
    if(edges) {
        if(path.starts_with("source.")){scope=Scope::Source;name=path.substr(7);}
        else if(path.starts_with("target.")){scope=Scope::Target;name=path.substr(7);}
        else if(path.starts_with("edge."))name=path.substr(5);
    }
    const bool vertex=scope!=Scope::Edge;
    const auto& fields=metadata_.at(vertex?"vertex_fields":"edge_fields");
    expect(fields.contains(name),"unknown "+std::string(vertex?"vertex":"edge")+" field: "+path);
    return {scope,name,fields.at(name).at("type").get<std::string>(),vertex?name=="id":name=="source" || name=="target"};
}
std::pair<std::size_t,std::size_t> QueryEngine::endpoints(std::size_t row) const {
    const auto& edge=graph_.edge(khypergraph::EdgeId{row});
    auto a=edge.endpoints[0].vertex.value,b=edge.endpoints[1].vertex.value;
    if(directed_) {if(edge.endpoints[0].role==khypergraph::EndpointRole::Head)std::swap(a,b);}
    else if(ids_[a]>ids_[b])std::swap(a,b);
    return {a,b};
}
Value QueryEngine::value(std::size_t row,const Field& field) const {
    if(field.scope==Scope::Edge) {
        if(field.name=="source" || field.name=="target") {const auto [a,b]=endpoints(row);return ids_[field.name=="source"?a:b];}
        return graph_.edge(khypergraph::EdgeId{row}).properties.at(field.name);
    }
    if(field.scope==Scope::Source || field.scope==Scope::Target){const auto [a,b]=endpoints(row);row=field.scope==Scope::Source?a:b;}
    if(field.name=="id")return ids_[row];
    return graph_.vertex(khypergraph::VertexId{row}).properties.at(field.name);
}
json QueryEngine::encode(const Value& input,bool id) {
    return std::visit([id](const auto& v)->json {
        using T=std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T,std::monostate>)return nullptr;
        else if constexpr(std::is_same_v<T,I>){if(id)return std::to_string(v);else return v;}
        else return v;
    },input);
}
int QueryEngine::compare(const Value& a,const Value& b) {
    expect(a.index()==b.index(),"incompatible value types");
    return std::visit([&](const auto& x)->int {
        using T=std::decay_t<decltype(x)>;
        if constexpr(std::is_same_v<T,std::monostate>)return 0;
        else {const auto& y=std::get<T>(b);return x<y?-1:x>y?1:0;}
    },a);
}
Value QueryEngine::literal(const json& input,const Field& field) {
    if(field.type=="int64") {
        if(input.is_number_integer()) {
            if(input.is_number_unsigned())expect(input.get<std::uint64_t>()<=static_cast<std::uint64_t>(std::numeric_limits<I>::max()),"int64 literal overflow");
            return input.get<I>();
        }
        expect(input.is_string(),field.name+": int64 filter values must be integers or exact decimal strings");
        const auto text=input.get<std::string>();
        const auto first=!text.empty() && text.front()=='-'?1u:0u;
        expect(first<text.size() && (text[first]!='0' || text.size()==1) && text.find_first_not_of("0123456789",first)==std::string::npos,"invalid decimal int64 filter literal");
        I number=0;const auto parsed=std::from_chars(text.data(),text.data()+text.size(),number);
        expect(parsed.ec==std::errc{} && parsed.ptr==text.data()+text.size(),"int64 filter literal overflow");return number;
    }
    if(field.type=="float64") {expect(input.is_number(),"float64 filter value must be numeric");const auto number=input.get<double>();expect(std::isfinite(number),"non-finite filter value");return number;}
    if(field.type=="bool"){expect(input.is_boolean(),"bool filter value must be true or false");return input.get<bool>();}
    expect(input.is_string(),"string filter value must be a string");return input.get<std::string>();
}
QueryEngine::Predicate QueryEngine::filter(const json& conditions,bool edges) const {
    expect(conditions.is_array() && conditions.size()<=16,"filter must contain at most 16 AND conditions");
    std::vector<Predicate> tests;
    for(const auto& item:conditions) {
        const auto field=resolve(item.at("field").get<std::string>(),edges);const auto op=item.at("op").get<std::string>();
        if(op=="is_null" || op=="is_not_null") {
            expect(!item.contains("value"),"null predicates must omit value");
            tests.push_back([this,field,op](std::size_t row){return null(value(row,field))==(op=="is_null");});continue;
        }
        expect(item.contains("value"),"comparison requires value; use is_null for nulls");
        std::vector<Value> values;
        if(op=="in") {expect(item["value"].is_array() && !item["value"].empty() && item["value"].size()<=50,"in requires 1..50 values");for(const auto& v:item["value"])values.push_back(literal(v,field));}
        else {expect(op=="eq" || op=="ne" || op=="gt" || op=="gte" || op=="lt" || op=="lte","unsupported filter operator");values.push_back(literal(item["value"],field));}
        expect(field.type!="bool" || op=="eq" || op=="ne" || op=="in","bool supports only eq, ne, in and null tests");
        tests.push_back([this,field,op,values=std::move(values)](std::size_t row){
            const auto current=value(row,field);if(null(current))return false;
            if(op=="in")return std::any_of(values.begin(),values.end(),[&](const auto& v){return compare(current,v)==0;});
            const auto c=compare(current,values[0]);
            return op=="eq"?c==0:op=="ne"?c!=0:op=="gt"?c>0:op=="gte"?c>=0:op=="lt"?c<0:c<=0;
        });
    }
    return [tests=std::move(tests)](std::size_t row){return std::all_of(tests.begin(),tests.end(),[&](const auto& test){return test(row);});};
}
QueryEngine::Predicate QueryEngine::selection(const json& p,bool edges) const {
    const auto own=filter(p.value("filter",json::array()),edges);
    if(!edges)return own;
    const auto source=filter(p.value("source_filter",json::array()),false),target=filter(p.value("target_filter",json::array()),false);
    if(p.value("source_filter",json::array()).empty() && p.value("target_filter",json::array()).empty())return own;
    return [this,own,source,target](std::size_t row){if(!own(row))return false;const auto [a,b]=endpoints(row);return source(a) && target(b);};
}
bool QueryEngine::handles(const std::string& op) {
    return op=="get_dataset_schema" || op=="profile_fields" || op=="query_vertices" || op=="query_edges" || op=="aggregate_vertices" || op=="aggregate_edges";
}
json QueryEngine::call(const std::string& op,const json& p) const {
    if(op=="get_dataset_schema")return schema(p);
    if(op=="profile_fields")return profile(p);
    if(op=="query_vertices" || op=="query_edges")return query(p,op=="query_edges");
    return aggregate(p,op=="aggregate_edges");
}
json QueryEngine::schema(const json& p) const {
    const auto table=entity(p);json rows=json::array();
    for(auto it=metadata_.at(table=="vertices"?"vertex_fields":"edge_fields").begin();it!=metadata_.at(table=="vertices"?"vertex_fields":"edge_fields").end();++it) {
        auto row=it.value();row["field"]=it.key();row["query_field"]=query_name(it.key(),table=="edges");rows.push_back(std::move(row));
    }
    return {{"entity",table},{"rows",rows},{"total_matches",rows.size()},{"truncated",false},{"nullable_meaning","allowed by the format; use profile_fields for actual null counts"},{"endpoint_order",directed_?"directed_source_target":"canonical_external_id_ascending"}};
}
json QueryEngine::profile(const json& p) const {
    const auto table=entity(p);const bool edges=table=="edges";
    json names=p.value("fields",json::array());const auto& definitions=metadata_.at(edges?"edge_fields":"vertex_fields");
    if(names.empty())for(auto it=definitions.begin();it!=definitions.end() && names.size()<16;++it)names.push_back(query_name(it.key(),edges));
    expect(names.is_array() && names.size()<=16,"profile supports up to 16 fields");
    const int sample=p.value("example_limit",3);expect(sample>=1 && sample<=5,"example_limit must be 1..5");
    const auto count=edges?graph_.edges().size():ids_.size();json rows=json::array();
    for(const auto& name:names) {
        const auto field=resolve(name.get<std::string>(),edges);std::size_t missing=0;json examples=json::array();
        for(std::size_t i=0;i<count;++i) {
            const auto v=value(i,field);if(null(v)){++missing;continue;}
            if(examples.size()<static_cast<std::size_t>(sample)){const auto encoded=encode(v,field.id);if(std::find(examples.begin(),examples.end(),encoded)==examples.end())examples.push_back(encoded);}
        }
        rows.push_back({{"field",name},{"type",field.type},{"null_count",missing},{"non_null_count",count-missing},{"null_fraction",count?json(static_cast<double>(missing)/count):json(nullptr)},{"examples",examples}});
    }
    return {{"entity",table},{"rows",rows},{"total_records",count},{"examples_scope","first distinct non-null values in load order; not a random or representative sample"},{"truncated",!p.contains("fields") && definitions.size()>names.size()}};
}

json QueryEngine::query(const json& p,bool edges) const {
    const auto selected=selection(p,edges);json names=p.value("fields",json::array());
    const auto& definitions=metadata_.at(edges?"edge_fields":"vertex_fields");
    if(names.empty())for(auto it=definitions.begin();it!=definitions.end() && names.size()<16;++it)names.push_back(query_name(it.key(),edges));
    expect(names.size()<=16,"query supports up to 16 fields");std::vector<Field> columns;
    for(const auto& name:names)columns.push_back(resolve(name.get<std::string>(),edges));
    struct Sort {Field field;bool descending;};std::vector<Sort> sort;
    const auto ordering=p.value("order_by",json::array());expect(ordering.size()<=2,"at most two sort keys");
    for(const auto& key:ordering){const auto direction=key.value("direction","asc");expect(direction=="asc" || direction=="desc","invalid sort direction");sort.push_back({resolve(key.at("field").get<std::string>(),edges),direction=="desc"});}
    std::vector<std::size_t> matches;const auto count=edges?graph_.edges().size():ids_.size();
    for(std::size_t i=0;i<count;++i)if(selected(i))matches.push_back(i);
    const auto less=[&](std::size_t a,std::size_t b){
        for(const auto& key:sort){const auto av=value(a,key.field),bv=value(b,key.field);if(null(av)!=null(bv))return !null(av);if(null(av))continue;const auto c=compare(av,bv);if(c)return key.descending?c>0:c<0;}
        if(!edges)return ids_[a]<ids_[b];
        const auto [as,at]=endpoints(a);const auto [bs,bt]=endpoints(b);return std::pair{ids_[as],ids_[at]}<std::pair{ids_[bs],ids_[bt]};
    };
    const auto take=std::min(matches.size(),static_cast<std::size_t>(row_limit(p)));
    std::partial_sort(matches.begin(),matches.begin()+take,matches.end(),less);
    json rows=json::array();for(std::size_t k=0;k<take;++k){json row=json::object();for(std::size_t j=0;j<columns.size();++j)row[names[j].get<std::string>()]=encode(value(matches[k],columns[j]),columns[j].id);rows.push_back(std::move(row));}
    return {{"rows",rows},{"entity",edges?"edges":"vertices"},{"total_matches",matches.size()},{"total_records",count},{"truncated",take<matches.size()},{"fields_truncated",!p.contains("fields") && definitions.size()>names.size()},{"scope","filtered records from the full graph"},{"endpoint_order",directed_?"directed_source_target":"canonical_external_id_ascending"},{"null_order","last, for both ascending and descending"}};
}

json QueryEngine::aggregate(const json& p,bool edges) const {
    const auto selected=selection(p,edges);
    const auto group_names=p.value("group_by",json::array());expect(group_names.size()<=3,"at most three group fields");
    std::vector<Field> group_fields;std::set<std::string> output_names;
    for(const auto& name:group_names){expect(output_names.insert(name.get<std::string>()).second,"duplicate group field");group_fields.push_back(resolve(name.get<std::string>(),edges));}
    struct Agg {std::string op,alias;std::optional<Field> field;};std::vector<Agg> aggregations;
    expect(p.at("aggregations").is_array() && !p["aggregations"].empty() && p["aggregations"].size()<=8,"require 1..8 aggregations");
    for(const auto& item:p["aggregations"]) {
        const auto op=item.at("op").get<std::string>(),alias=item.at("as").get<std::string>();
        expect(op=="count" || op=="sum" || op=="avg" || op=="min" || op=="max","unsupported aggregation");
        expect(!alias.empty() && output_names.insert(alias).second,"aggregation aliases must be unique and distinct from group fields");
        std::optional<Field> field;if(item.contains("field"))field=resolve(item["field"].get<std::string>(),edges);
        expect(op=="count" || field.has_value(),"aggregation requires field");
        if(op=="sum" || op=="avg")expect(field->type=="int64" || field->type=="float64","sum/avg require numeric fields");
        aggregations.push_back({op,alias,field});
    }
    struct Cell {std::uint64_t count=0;I integer_sum=0;long double real_sum=0;Value extremum;};
    struct Group {std::vector<Value> keys;std::vector<Cell> cells;};
    std::map<std::string,Group> groups;
    if(group_fields.empty())groups.emplace("[]",Group{{},std::vector<Cell>(aggregations.size())});
    const auto count=edges?graph_.edges().size():ids_.size();std::size_t matched=0;
    for(std::size_t row=0;row<count;++row) {
        if(!selected(row))continue;++matched;
        auto found=groups.begin();
        // A global aggregate has one fixed group; avoid serializing [] and doing
        // map lookups once per row on multi-million-edge graphs.
        if(!group_fields.empty()) {
            std::vector<Value> keys;json key=json::array();for(const auto& f:group_fields){
                auto item=value(row,f);
                // Equal floating zeros must share one group despite distinct JSON spellings.
                if(auto number=std::get_if<double>(&item);number && *number==0.0)*number=0.0;
                keys.push_back(std::move(item));key.push_back(encode(keys.back()));
            }
            const auto encoded=key.dump();
            found=groups.find(encoded);
            if(found==groups.end()){expect(groups.size()<100000,"group limit exceeded (100000); narrow filters or group by fewer categories");found=groups.emplace(encoded,Group{std::move(keys),std::vector<Cell>(aggregations.size())}).first;}
        }
        for(std::size_t j=0;j<aggregations.size();++j) {
            const auto& spec=aggregations[j];auto& cell=found->second.cells[j];
            const auto current=spec.field?value(row,*spec.field):Value{};
            if(spec.field && null(current))continue;
            ++cell.count;
            if(spec.op=="sum") {
                if(spec.field->type=="int64")cell.integer_sum=add(cell.integer_sum,std::get<I>(current));
                else cell.real_sum+=std::get<double>(current);
            } else if(spec.op=="avg")cell.real_sum+=spec.field->type=="int64"?static_cast<long double>(std::get<I>(current)):std::get<double>(current);
            else if(spec.op=="min" || spec.op=="max") {
                if(null(cell.extremum) || (spec.op=="min"?compare(current,cell.extremum)<0:compare(current,cell.extremum)>0))cell.extremum=current;
            }
        }
    }
    // Keep raw typed keys until sorting is finished, then encode ID fields.
    struct Result {json data;const Group* group;};std::vector<Result> results;
    for(const auto& [key,group]:groups) {
        json row=json::object();for(std::size_t j=0;j<group_fields.size();++j)row[group_names[j].get<std::string>()]=encode(group.keys[j]);
        for(std::size_t j=0;j<aggregations.size();++j) {
            const auto& spec=aggregations[j];const auto& cell=group.cells[j];json v;
            if(spec.op=="count")v=cell.count;
            else if(spec.op=="min" || spec.op=="max")v=encode(cell.extremum);
            else if(spec.op=="sum" && spec.field->type=="int64")v=cell.integer_sum;
            else if(spec.op=="avg" && !cell.count)v=nullptr;
            else {const auto number=static_cast<double>(spec.op=="avg"?cell.real_sum/cell.count:cell.real_sum);expect(std::isfinite(number),"floating-point aggregate overflow");v=number;}
            row[spec.alias]=v;
        }
        results.push_back({std::move(row),&group});
    }
    const auto ordering=p.value("order_by",json::array());expect(ordering.size()<=2,"at most two sort keys");
    for(const auto& key:ordering){expect(output_names.contains(key.at("field").get<std::string>()),"order_by must name a group field or aggregation alias");expect(key.value("direction","asc")=="asc" || key.value("direction","asc")=="desc","invalid sort direction");}
    const auto less=[&](const Result& a,const Result& b) {
        for(const auto& key:ordering) {
            const auto name=key.at("field").get<std::string>();const auto& av=a.data.at(name);const auto& bv=b.data.at(name);
            if(av.is_null()!=bv.is_null())return !av.is_null();if(av==bv || av.is_null())continue;
            const bool ascending=key.value("direction","asc")=="asc";return ascending?av<bv:bv<av;
        }
        for(std::size_t j=0;j<group_fields.size();++j){const auto& av=a.group->keys[j];const auto& bv=b.group->keys[j];if(null(av)!=null(bv))return !null(av);const auto c=compare(av,bv);if(c)return c<0;}
        return false;
    };
    const auto take=std::min(results.size(),static_cast<std::size_t>(row_limit(p)));
    std::partial_sort(results.begin(),results.begin()+take,results.end(),less);
    json rows=json::array();for(std::size_t k=0;k<take;++k) {
        auto row=results[k].data;
        for(std::size_t j=0;j<group_fields.size();++j)if(group_fields[j].id)row[group_names[j].get<std::string>()]=encode(results[k].group->keys[j],true);
        for(std::size_t j=0;j<aggregations.size();++j)if(aggregations[j].field && aggregations[j].field->id && (aggregations[j].op=="min" || aggregations[j].op=="max"))row[aggregations[j].alias]=encode(results[k].group->cells[j].extremum,true);
        rows.push_back(std::move(row));
    }
    return {{"rows",rows},{"entity",edges?"edges":"vertices"},{"matched_records",matched},{"total_groups",results.size()},{"truncated",take<results.size()},{"scope","aggregate all matching records before applying result limit; no graph mutation"},{"endpoint_order",directed_?"directed_source_target":"canonical_external_id_ascending"},{"null_policy","null keys form a group; field aggregations ignore nulls; count without field counts records"},{"avg_precision","float64, approximate"}};
}
