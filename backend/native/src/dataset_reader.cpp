#include "dataset_reader.hpp"
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/util/utf8.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/schema.h>
#include <parquet/file_reader.h>
#include <charconv>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>
#include <set>
#include <stdexcept>

namespace dataset {
using json = nlohmann::json;
using I = std::int64_t;
using Value = khypergraph::PropertyValue;
namespace fs = std::filesystem;

static void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
static void check(const arrow::Status& status) {
    if (!status.ok()) throw std::runtime_error(status.ToString());
}
template<class T> static T unwrap(arrow::Result<T> result) {
    check(result.status());
    return std::move(result).ValueOrDie();
}
static std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
static bool nonempty(const json& value) {
    return value.is_string() && value.get_ref<const std::string&>().find_first_not_of(" \t\r\n") != std::string::npos;
}
static void fields(const json& object, const std::set<std::string>& required,
                   const std::set<std::string>& allowed, const std::string& label) {
    require(object.is_object(), label + ": expected object");
    for (const auto& key : required) require(object.contains(key), label + ": missing " + key);
    for (auto it=object.begin(); it!=object.end(); ++it)
        require(allowed.contains(it.key()), label + ": unknown field " + it.key());
}

json read_descriptor(const fs::path& path) {
    arrow::util::InitializeUTF8();
    require(fs::is_regular_file(path) && fs::file_size(path)<=1000000, "invalid dataset descriptor");
    std::ifstream file(path, std::ios::binary);
    require(file.good(), "cannot open dataset descriptor");
    auto d=json::parse(file);
    fields(d,{"format","id","name","directed","vertices","edges"},
           {"format","id","name","directed","vertices","edges","description"},"descriptor");
    require(d["format"]=="khyper.graph.v1" && d["directed"].is_boolean(),"invalid format or directed flag");
    require(nonempty(d["id"]) && nonempty(d["name"]),"id and name must be nonempty strings");
    require(!d.contains("description") || d["description"].is_string(),"description must be a string");
    return d;
}

json metadata(const json& d) {
    return {{"id",d["id"]},{"name",d["name"]},{"directed",d["directed"]},
            {"description",d.value("description","")},
            {"weight_meaning",d["edges"]["weight_semantics"]},
            {"weight_unit",d["edges"].value("weight_unit","weight")},
            {"vertex_properties",d["vertices"].value("properties",json::object())},
            {"edge_properties",d["edges"].value("properties",json::object())}};
}

// Binary input preserves quoted newlines; BOM is consumed only at the header.
static bool tsv_record(std::istream& input, std::vector<std::string>& values) {
    values.clear(); std::string value; bool quoted=false, closed=false, started=false;
    for (;;) {
        const int next=input.get();
        if(next==std::char_traits<char>::eof()) {
            require(!quoted,"unterminated quoted TSV field");
            if(!started && value.empty() && values.empty()) return false;
            values.push_back(std::move(value));return true;
        }
        started=true;const char c=static_cast<char>(next);
        if(quoted) {
            if(c=='"') {
                if(input.peek()=='"'){input.get();value+='"';}
                else {quoted=false;closed=true;}
            } else value+=c;
            continue;
        }
        if(c=='\t') {values.push_back(std::move(value));value.clear();closed=false;continue;}
        if(c=='\n' || c=='\r') {
            if(c=='\r' && input.peek()=='\n')input.get();
            values.push_back(std::move(value));return true;
        }
        require(!closed,"unexpected character after quoted TSV field");
        if(c=='"') {require(value.empty(),"unexpected quote in TSV field");quoted=true;}
        else value+=c;
    }
}
static void skip_bom(std::istream& in) {
    if(in.peek()==0xef) {
        char bytes[3];in.read(bytes,3);
        require(in && static_cast<unsigned char>(bytes[1])==0xbb && static_cast<unsigned char>(bytes[2])==0xbf,"invalid UTF-8 BOM");
    }
}

struct Table::Impl {
    struct Field {std::string role,name,type; bool structural;};
    fs::path path;std::string kind,format;
    std::uintmax_t size;fs::file_time_type modified;
    std::vector<Field> selected;
    std::vector<std::string> header;
    std::unique_ptr<parquet::arrow::FileReader> parquet;
    std::uint64_t hint=0;

    std::string context(std::uint64_t ordinal, const Field& field) const {
        return kind+" record "+std::to_string(ordinal)+", "+field.name+": ";
    }
    Value validate(Value value, const Field& field, std::uint64_t ordinal) const {
        // Error context is expensive at millions of rows; build it only on failure.
        if(field.structural && (!std::holds_alternative<I>(value) || std::get<I>(value)<0))
            throw std::runtime_error(context(ordinal,field)+"expected non-null nonnegative int64");
        if(auto number=std::get_if<double>(&value);number && !std::isfinite(*number))
            throw std::runtime_error(context(ordinal,field)+"non-finite property");
        if(auto text=std::get_if<std::string>(&value);text && !arrow::util::ValidateUTF8(*text))
            throw std::runtime_error(context(ordinal,field)+"invalid UTF-8");
        return value;
    }
    Value text_value(const std::string& text, const Field& field, std::uint64_t ordinal) const {
        if(text.empty())return validate(std::monostate{},field,ordinal);
        Value value;
        const auto expect=[&](bool condition,std::string_view message) {
            if(!condition)throw std::runtime_error(context(ordinal,field)+std::string(message));
        };
        if(field.type=="string")value=text;
        else if(field.type=="bool") {expect(text=="true" || text=="false","invalid bool");value=text=="true";}
        else if(field.type=="int64") {
            const std::size_t first=text.front()=='-'?1:0;
            expect(first<text.size() && (text[first]!='0' || text.size()==1) && text.find_first_not_of("0123456789",first)==std::string::npos,"invalid decimal int64");
            I number=0;auto result=std::from_chars(text.data(),text.data()+text.size(),number);
            expect(result.ec==std::errc{} && result.ptr==text.data()+text.size(),"int64 overflow");value=number;
        } else {
            const auto first=text.find_first_not_of(" \t\r\n");
            expect(first!=std::string::npos,"invalid float64");
            auto numeric=std::string_view(text).substr(first,text.find_last_not_of(" \t\r\n")-first+1);
            if(numeric.front()=='+')numeric.remove_prefix(1);
            expect(!numeric.empty(),"invalid float64");
            double number=0;auto result=std::from_chars(numeric.data(),numeric.data()+numeric.size(),number);
            expect(result.ec==std::errc{} && result.ptr==numeric.data()+numeric.size(),"invalid float64");value=number;
        }
        return validate(std::move(value),field,ordinal);
    }
    Value arrow_value(const arrow::Array& array, const Field& field, std::int64_t row, std::uint64_t ordinal) const {
        Value value;
        if(!array.IsNull(row)) switch(array.type_id()) {
            case arrow::Type::INT64:value=static_cast<const arrow::Int64Array&>(array).Value(row);break;
            case arrow::Type::DOUBLE:value=static_cast<const arrow::DoubleArray&>(array).Value(row);break;
            case arrow::Type::BOOL:value=static_cast<const arrow::BooleanArray&>(array).Value(row);break;
            case arrow::Type::STRING:value=std::string(static_cast<const arrow::StringArray&>(array).GetView(row));break;
            case arrow::Type::LARGE_STRING:value=std::string(static_cast<const arrow::LargeStringArray&>(array).GetView(row));break;
            default:throw std::runtime_error("unsupported Arrow value");
        }
        return validate(std::move(value),field,ordinal);
    }
};

Table::Table(const json& spec, const fs::path& base, const std::string& kind):impl_(std::make_unique<Impl>()) {
    auto& t=*impl_;t.kind=kind;
    auto allowed=std::set<std::string>{"path","format","columns","properties","types"};
    if(kind=="edges"){allowed.insert("weight_semantics");allowed.insert("weight_unit");}
    fields(spec,{"path","format","columns"},allowed,kind);
    require(nonempty(spec["path"]) && (spec["format"]=="parquet" || spec["format"]=="tsv"),kind+": invalid path or format");
    const auto path_text=spec["path"].get<std::string>();
    t.path=fs::weakly_canonical(base/fs::path(std::u8string(path_text.begin(),path_text.end())));
    require(fs::is_regular_file(t.path),kind+": source is not a regular local file");
    t.size=fs::file_size(t.path);t.modified=fs::last_write_time(t.path);t.format=spec["format"];
    const bool vertices=kind=="vertices";
    const auto roles=vertices?std::set<std::string>{"id","name","type"}:std::set<std::string>{"source","target","weight","type"};
    fields(spec["columns"],vertices?std::set<std::string>{"id"}:std::set<std::string>{"source","target"},roles,kind+".columns");
    const auto props=spec.value("properties",json::object());require(props.is_object(),kind+": invalid properties mapping");
    std::set<std::string> names;
    for(const auto& mapping:{spec["columns"],props}) for(auto it=mapping.begin();it!=mapping.end();++it) {
        require(!it.key().empty() && nonempty(it.value()),kind+": empty column mapping");
        const std::string name=it.value();require(names.insert(name).second,kind+": duplicate physical mapping "+name);
        const bool structural=vertices?it.key()=="id":(it.key()=="source" || it.key()=="target" || it.key()=="weight");
        t.selected.push_back({it.key(),name,"",structural});
    }
    for(auto it=props.begin();it!=props.end();++it)require(!roles.contains(it.key()),kind+": property collides with structural role");
    if(!vertices) {
        require(spec.contains("weight_semantics") && nonempty(spec["weight_semantics"]),"edges.weight_semantics is required");
        require(!spec.contains("weight_unit") || nonempty(spec["weight_unit"]),"invalid weight_unit");
    }
    if(t.format=="parquet") {
        auto file=unwrap(arrow::io::ReadableFile::Open(utf8(t.path)));
        t.parquet=unwrap(parquet::arrow::OpenFile(file,arrow::default_memory_pool()));
        t.parquet->set_batch_size(32768);t.parquet->set_use_threads(true);
        std::shared_ptr<arrow::Schema> schema;check(t.parquet->GetSchema(&schema));
        std::set<std::string> unique;
        for(const auto& field:schema->fields())require(unique.insert(field->name()).second,kind+": duplicate Parquet column "+field->name());
        for(auto& field:t.selected) {
            auto column=schema->GetFieldByName(field.name);require(column!=nullptr,kind+": missing Parquet column "+field.name);
            switch(column->type()->id()) {
                case arrow::Type::INT64:field.type="int64";break;
                case arrow::Type::DOUBLE:field.type="float64";break;
                case arrow::Type::BOOL:field.type="bool";break;
                case arrow::Type::STRING:case arrow::Type::LARGE_STRING:field.type="string";break;
                default:throw std::runtime_error(kind+", "+field.name+": unsupported type "+column->type()->ToString());
            }
        }
        t.hint=t.parquet->parquet_reader()->metadata()->num_rows();
    } else {
        require(spec.contains("types") && spec["types"].is_object() && spec["types"].size()==names.size(),kind+": TSV types must cover all mapped columns");
        std::ifstream input(t.path,std::ios::binary);require(input.good(),kind+": cannot read TSV");skip_bom(input);
        require(tsv_record(input,t.header),kind+": missing TSV header");std::set<std::string> unique;
        for(const auto& name:t.header) {
            require(arrow::util::ValidateUTF8(name),kind+": invalid UTF-8 TSV header");
            require(unique.insert(name).second,kind+": duplicate TSV header");
        }
        for(auto& field:t.selected) {
            require(spec["types"].contains(field.name) && spec["types"][field.name].is_string(),kind+": missing TSV type "+field.name);
            field.type=spec["types"][field.name];
            require(field.type=="int64" || field.type=="float64" || field.type=="bool" || field.type=="string",kind+": unsupported TSV type");
            require(unique.contains(field.name),kind+": missing TSV column "+field.name);
        }
    }
    for(const auto& field:t.selected)if(spec["columns"].contains(field.role))
        require(field.type==(field.structural?"int64":"string"),kind+", "+field.name+": incompatible structural type");
}
Table::~Table()=default;
std::uint64_t Table::count_hint() const{return impl_->hint;}
nlohmann::json Table::schema() const {
    json fields=json::object();
    for(const auto& field:impl_->selected)
        fields[field.role]={{"type",field.type},{"column",field.name},{"nullable",!field.structural},{"implicit",false}};
    if(impl_->kind=="edges" && !fields.contains("weight"))
        fields["weight"]={{"type","int64"},{"column",nullptr},{"nullable",false},{"implicit",true}};
    return fields;
}
void Table::check_unchanged() const {
    const auto& t=*impl_;
    require(fs::file_size(t.path)==t.size && fs::last_write_time(t.path)==t.modified,t.kind+": source file changed during loading; retry");
}
void Table::read(const std::function<void(Record&&)>& consume,const Progress& progress) {
    auto& t=*impl_;std::uint64_t ordinal=0;progress(t.kind,0);
    auto finish=[&](Record&& record){
        if(t.kind=="edges" && !record.contains("weight"))record["weight"]=I{1};
        try {consume(std::move(record));}
        catch(const std::exception& e){throw std::runtime_error(t.kind+" record "+std::to_string(ordinal)+": "+e.what());}
    };
    if(t.parquet) {
        std::vector<int> groups(t.parquet->num_row_groups());std::iota(groups.begin(),groups.end(),0);
        std::vector<int> columns;
        for(const auto& field:t.selected)for(const auto& node:t.parquet->manifest().schema_fields)
            if(node.field->name()==field.name) {require(node.is_leaf(),"mapped Parquet field is not scalar");columns.push_back(node.column_index);break;}
        auto reader=unwrap(t.parquet->GetRecordBatchReader(groups,columns));
        while(auto batch=unwrap(reader->Next())) {
            std::vector<std::shared_ptr<arrow::Array>> arrays;
            for(const auto& field:t.selected) {auto array=batch->GetColumnByName(field.name);require(array!=nullptr,"projected Parquet column missing");arrays.push_back(std::move(array));}
            for(std::int64_t row=0;row<batch->num_rows();++row) {
                ++ordinal;Record record;record.reserve(t.selected.size());
                for(std::size_t i=0;i<t.selected.size();++i)record.emplace(t.selected[i].role,t.arrow_value(*arrays[i],t.selected[i],row,ordinal));
                finish(std::move(record));
            }
            progress(t.kind,ordinal);
        }
    } else {
        std::ifstream input(t.path,std::ios::binary);require(input.good(),t.kind+": cannot read TSV");skip_bom(input);
        std::vector<std::string> values;require(tsv_record(input,values) && values==t.header,t.kind+": TSV header changed during loading");
        std::vector<std::size_t> positions;
        for(const auto& field:t.selected)positions.push_back(std::find(t.header.begin(),t.header.end(),field.name)-t.header.begin());
        for(;;) {
            bool available;
            try {available=tsv_record(input,values);}catch(const std::exception& e){throw std::runtime_error(t.kind+" record "+std::to_string(ordinal+1)+": "+e.what());}
            if(!available)break;
            ++ordinal;
            if(values.size()!=t.header.size())throw std::runtime_error(t.kind+" record "+std::to_string(ordinal)+": TSV field count mismatch");
            for(const auto& value:values)if(!arrow::util::ValidateUTF8(value))
                throw std::runtime_error(t.kind+" record "+std::to_string(ordinal)+": invalid UTF-8 TSV field");
            Record record;record.reserve(t.selected.size());
            for(std::size_t i=0;i<t.selected.size();++i)record.emplace(t.selected[i].role,t.text_value(values[positions[i]],t.selected[i],ordinal));
            finish(std::move(record));if(ordinal%32768==0)progress(t.kind,ordinal);
        }
        require(!input.bad(),t.kind+": TSV read failure");progress(t.kind,ordinal);
    }
    check_unchanged();
}
}
