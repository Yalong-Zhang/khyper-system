# KHyper Ordinary Graph Dataset Format, Version 1

**Status: implemented external import contract.** The application selects descriptors, reads external Parquet/TSV files in place, validates them, and loads them into the C++ graph without a disk conversion step.

## 1. Purpose and scope

A user prepares a dataset and selects its descriptor file in the local application. KHyper opens the referenced source files in place and builds its in-memory graph. Selecting a dataset MUST NOT copy its data into the project, rewrite the source files, or require an application-generated intermediate dataset.

Version 1 supports finite, homogeneous-directedness, ordinary property graphs: directed or undirected, optionally weighted, with one vertex table and one edge table. Each edge has exactly two endpoints. Hyperedges, mixed directed/undirected edges, temporal queries, remote object stores, and SQL/database sources are outside this version.

The words **MUST**, **MUST NOT**, **SHOULD**, and **MAY** state requirements of this contract. They are not descriptions of features already implemented.

The design separates **logical field roles** from **physical column names**. Existing talent-flow, OpenAIRE coauthor, and DBLP coauthor files can therefore retain their names, schemas, and locations.

## 2. Formal graph model

A dataset represents

\[
G=(V,E,d,w,\mu_V,\mu_E),
\]

where:

- \(V\) is the finite set of externally identified vertices.
- \(d\in\{\mathrm{directed},\mathrm{undirected}\}\) applies to every edge.
- For a directed graph, \(E\subseteq\{(u,v):u,v\in V,u\ne v\}\).
- For an undirected graph, \(E\subseteq\{\{u,v\}:u,v\in V,u\ne v\}\).
- \(w:E\to\mathbb Z_{\ge0}\) is an optional nonnegative integer weight function. When no weight column is mapped, \(w(e)=1\).
- \(\mu_V\) and \(\mu_E\) attach optional scalar properties to vertices and edges.

**Version 1 requires a simple graph:** no self-loops and no duplicate logical edges. This is a file-level requirement, not a request for the importer to clean a multigraph. Users needing event-level or parallel-edge data must aggregate it before selecting it in the application. A directed reciprocal pair \((u,v),(v,u)\) consists of two distinct edges and is valid.

The vertex table is authoritative. A vertex with no incident edges is retained. The importer MUST NOT infer the complete vertex set from the edge table, silently drop isolated vertices, or silently create missing endpoint vertices.

An empty edge table with a valid schema is allowed. A vertex table with no rows is also allowed, provided the edge table is empty. Algorithms that require nonempty input must report their own preconditions.

## 3. The dataset descriptor

The selection entry point is a UTF-8 JSON file, conventionally named `dataset.json`. A different filename is allowed. It describes the existing files; it does not contain their data.

Required top-level fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `format` | string | Exactly `khyper.graph.v1`. |
| `id` | string | Nonempty dataset identifier, stable across ordinary uses. |
| `name` | string | Nonempty human-readable dataset name. |
| `directed` | boolean | Explicit directedness; never inferred from column names or reciprocal rows. |
| `vertices` | object | Vertex file, structural column mappings, optional properties. |
| `edges` | object | Edge file, structural column mappings, weight interpretation, optional properties. |

Optional top-level `description` is a string describing provenance and analytical scope. Unknown fields MUST be rejected in version 1, rather than silently interpreted. An implementation may introduce a new format version for additional features.

Dataset identifiers distinguish user-facing datasets; they are not content hashes. Counts are computed during loading. Metadata from an existing `metadata.json` or `manifest.json` MAY help a user author the descriptor, but is not automatically trusted as an import schema or validation result.

### 3.1 File specification

Each of `vertices` and `edges` MUST contain:

- `path`: a nonempty filesystem path string.
- `format`: exactly `parquet` or `tsv`.
- `columns`: an object mapping logical structural roles to physical column names.

Vertex `columns` allows `id` (required), `name` (optional), and `type` (optional). Edge `columns` allows `source` and `target` (both required), plus `weight` and `type` (both optional).

Both objects MAY include `properties`, an object mapping exposed property names to physical column names. Only mapped properties are exposed to analysis. Unmapped extra physical columns are allowed and ignored. Property names MUST be nonempty and MUST NOT collide with the structural roles of the same table. Each physical column SHOULD be mapped to at most one role/property.

Names and type labels are data. They MUST NOT be treated as commands or agent instructions. If `name` is omitted or null for a row, its decimal external ID is the display fallback. Duplicate display names are allowed; IDs still distinguish vertices. `type` values, when present, are strings; missing values mean unspecified type. A type label does not change directedness or graph structure.

### 3.2 Paths and selection behavior

- Relative paths resolve against the **descriptor file's directory**, never the project directory or process working directory.
- Absolute paths are allowed. Windows JSON examples SHOULD use forward slashes, such as `C:/graphs/example/vertices.parquet`; backslashes require JSON escaping.
- Relative paths containing `..` are allowed for references to already prepared files elsewhere on the same machine. This is a local-file reference format, not a zipped upload format.
- Version 1 references regular local files, not HTTP URLs. The files must be readable by the backend process.
- The descriptor can live beside the data or in a separate user-managed location. No fixed vertex or edge filename is required.
- The UI MUST select/reference a backend-accessible path. A standard browser upload control does not supply a trustworthy absolute local path; copying uploaded bytes is not an implementation of this selection contract. A native picker or local backend path selector is appropriate.
- The application opens the files read-only. Validation, parsing, external-to-internal ID mapping, and construction of adjacency/incidence indexes are normal **loading** operations. Exporting a normalized table, converting TSV to Parquet, or generating a persistent binary snapshot is **preprocessing** and MUST NOT be mandatory for this selection flow.

Opening a dataset may take time. The UI must distinguish selection, validation/loading, ready, and failed states. A new selection becomes active only after successful loading; a failed selection must not leave a partially active graph. In-flight analyses must continue against one immutable dataset instance or be explicitly stopped before switching. An implementation may release the old graph before loading a new one to control memory use, but must then clearly report that no dataset is active until loading succeeds.

Files MUST remain unchanged while a graph is loading. The application SHOULD compare file metadata before and after loading to detect ordinary concurrent edits. Version 1 does not promise a transactional snapshot across externally edited files.

## 4. Vertex identifiers

### 4.1 Storage domain

Every vertex ID MUST be a non-null integer in

\[
0\le\operatorname{id}\le2^{63}-1.
\]

IDs MUST be unique within the dataset. They need not start at zero, be dense, be sorted, or equal row positions. Edge endpoints use the same external ID domain, and every endpoint MUST resolve to exactly one vertex-table row.

Parquet structural IDs MUST have physical Arrow/Parquet `int64` type. Booleans, floating-point IDs, and numeric-looking string columns are not implicitly coerced. TSV structural IDs use canonical unsigned decimal text: `0` or a nonzero digit followed by zero or more digits, within the range above. No exponent notation, thousands separators, leading plus sign, or surrounding whitespace is accepted.

### 4.2 API and browser representation

**External IDs MUST be serialized as decimal strings in browser and agent-tool JSON interfaces.** Some existing OpenAIRE IDs exceed \(2^{53}-1\), the maximum integer exactly representable by a JavaScript `Number`.

For example, an external ID is transmitted as `"614613821311819618"`, not as a JSON number. The backend parses and range-checks it as an integer. It MUST NOT round-trip through a floating-point number.

The internal KHyperGraph ID MAY be dense and insertion-based. The loader MUST retain a reversible external/internal mapping. Internal IDs MUST NOT replace external IDs in reports or public tool parameters. ID namespaces are dataset-local; the same integer in two different datasets does not identify the same entity.

This string-serialization rule also applies to exact integer metric results outside the JavaScript safe-integer range. Weight storage and calculations remain integer-based; only their JSON representation changes when necessary.

## 5. Edge semantics and weights

### 5.1 Directed graphs

`source` is the origin/tail and `target` is the destination/head. One row stores one directed relationship. An edge \((u,v)\) does not imply \((v,u)\).

Unweighted in-degree and out-degree count distinct incoming and outgoing neighbors. Total degree is their sum. The number of distinct partners across both directions is the size of their union and is a separate metric.

Weighted inflow and outflow are sums of the corresponding edge weights. Total activity is inflow plus outflow; net inflow is inflow minus outflow. These generic structural metrics do not, by themselves, establish a domain-specific interpretation such as employment quality or company growth.

### 5.2 Undirected graphs

One row stores one unordered pair. The file MUST store each pair only once. It MAY use either endpoint order; the loader identifies a pair by \((\min(u,v),\max(u,v))\) in memory. Sorted rows and `source < target` are recommended, but not required.

If both `(u,v)` and `(v,u)` occur, they are duplicate edges and loading fails. The importer MUST NOT double weights or silently collapse a symmetric adjacency-list export.

Each edge contributes one to the degree of each endpoint, and its weight to the strength of each endpoint. Inflow, outflow, sources, destinations, and net inflow are not defined for an undirected dataset; the UI and tools MUST use neighbors, degree, and strength instead. A coauthor relationship must not be described as directional talent flow.

### 5.3 Weight declaration

`edges` MUST include `weight_semantics`, a nonempty English or user-language string stating what one unit means and how weights were aggregated. It MAY include `weight_unit`, a nonempty string such as `flow_weight`, `shared_publications`, or `edge`.

If `columns.weight` is present:

- Every row MUST contain a non-null `int64` weight in \([0,2^{63}-1]\).
- TSV weights use the same decimal lexical rules as IDs.
- Zero-weight edges are permitted. They count toward unweighted degree but contribute zero strength.
- Negative, fractional, infinite, NaN, Boolean, and missing weights are invalid. Version 1 intentionally does not support floating weights or infer missing weights as 1.

If `columns.weight` is omitted, every edge has unit weight 1. `weight_semantics` MUST describe this unweighted interpretation; the importer must not guess a weight column from its name.

The implementation MUST detect integer overflow in weight sums and derived integer statistics, or use wider exact arithmetic. Wrapping integers and silently switching to lossy floating-point arithmetic are prohibited. A proportion with zero total weight is undefined and MUST be returned as null, not NaN or infinity.

## 6. Physical file encodings

### 6.1 Parquet

Parquet is the preferred format for already prepared large graphs. A descriptor references one Parquet file per table, not a directory of partitions in version 1.

Mapped structural columns MUST exist with the types specified above. Names and type labels MUST be strings. Optional property columns may be nullable scalar Boolean, `int64`, finite `float64`, or UTF-8 string values. Nested structs, lists, maps, binary objects, timestamps, and other types are not automatically flattened or converted in version 1. Providers must prepare such properties as supported scalar columns before selection, or leave them unmapped.

Null optional properties remain null and are not replaced by an empty string or zero. An invalid mapped property value causes a validation error. Duplicate physical column names are invalid.

### 6.2 TSV

A TSV table MUST be UTF-8, have exactly one header row, and use the literal tab character as its delimiter. LF and CRLF line endings are accepted. A UTF-8 BOM is allowed only before the header.

The optional table-level `quoting` setting selects the TSV dialect:

- `"quoting": "csv"` (default): double quotes enclose fields containing tabs, quotes, or line breaks, and embedded double quotes are doubled.
- `"quoting": "none"`: fields are separated by literal tabs and records by LF or CRLF; all double quotes are ordinary characters and are preserved. Fields cannot contain literal tabs or line breaks. Use this for raw tab-separated exports such as the existing DBLP files, whose author names include `Felix "FX" Lindner`.

The setting applies to both the header and data rows, is configured independently for each table, and is not accepted for Parquet. Comments and additional pre-header metadata lines are not allowed. Each parsed record MUST have the same field count as the header, and header names MUST be unique. Blank data records are invalid; a final line terminator is allowed.

For `format: tsv`, the table specification MUST additionally contain `types`, mapping **every mapped physical column** to one of `int64`, `float64`, `bool`, or `string`. Structural IDs and weights use `int64`; names and type labels use `string`.

Optional-property empty TSV fields denote null. A null vertex name uses the ID fallback. Literal strings such as `NA` and `null` are ordinary strings, not implicit null markers. Structural ID/endpoint/weight fields MUST NOT be empty. Booleans are exactly `true` or `false`. Finite float text uses decimal/scientific notation without thousands separators. An empty string property and a null property are intentionally not distinguished by this TSV profile; use Parquet when that distinction matters.

Extra unmapped TSV columns may exist and are ignored. Parsing MUST follow the declared quoting mode; CSV-style quoted fields can span physical lines, while unquoted TSV preserves literal quote characters.

## 7. Descriptors for the existing datasets

These examples were checked against the actual headers/schemas in the workspace. They demonstrate field compatibility, not an assertion that every row of every large dataset has undergone full validation during documentation work. Paths assume that the descriptor is placed in the corresponding dataset directory. If the descriptor lives elsewhere, adjust paths without moving the data.

### 7.1 Existing directed company talent-flow graph

The bundled example places `directed-example.json` alongside both Parquet tables under `examples/datasets/`. The descriptor resolves each table by its filename.

```json
{
  "format": "khyper.graph.v1",
  "id": "company-talent-flow",
  "name": "Company Talent Flow",
  "description": "Existing company/organization flow graph. No event-time or person-level uniqueness is inferred from these aggregate files.",
  "directed": true,
  "vertices": {
    "path": "company.parquet",
    "format": "parquet",
    "columns": {"id": "company_rid", "name": "company_name"},
    "properties": {
      "industry": "rics_k50",
      "hq_country": "hq_country",
      "hq_state": "hq_state",
      "hq_metro_area": "hq_metro_area"
    }
  },
  "edges": {
    "path": "company_SwithTo_company.parquet",
    "format": "parquet",
    "columns": {"source": "src_group_rid", "target": "dst_group_rid", "weight": "weight"},
    "weight_unit": "flow_weight",
    "weight_semantics": "The existing aggregated weight for the ordered origin-to-destination company pair; sums across pairs are not globally deduplicated people."
  }
}
```

Preserve the actual filename spelling `SwithTo`. Existing company aliases are an application-level entity-resolution aid, not part of the graph schema, and MUST NOT carry over to a different dataset unless separately configured for it.

### 7.2 Existing undirected OpenAIRE coauthor graph

Directory: `dataset/processed/openaire_2018_2026_coauthor`.

```json
{
  "format": "khyper.graph.v1",
  "id": "openaire-2018-2026-coauthor",
  "name": "OpenAIRE Coauthorship, 2018–2026",
  "description": "Authors connected by shared research products; existing stable author IDs are retained.",
  "directed": false,
  "vertices": {
    "path": "vertices.parquet",
    "format": "parquet",
    "columns": {"id": "vertex_id", "name": "name", "type": "vertex_type"},
    "properties": {"original_id": "original_id", "orcid": "orcid", "organization": "organization", "country": "country"}
  },
  "edges": {
    "path": "edges.parquet",
    "format": "parquet",
    "columns": {"source": "source_id", "target": "destination_id", "weight": "weight", "type": "relation_type"},
    "weight_unit": "shared_research_products",
    "weight_semantics": "Number of distinct included collaborative research products shared by the unordered author pair; each product contributes once per pair."
  }
}
```

The inspected files contain 198,393 vertices and 330,373 edges. Existing `original_id` strings remain properties; they do not replace `vertex_id` as the structural key. An author's organization or country is a property with the source's own scope, not evidence about their location at a particular publication date.

### 7.3 Existing undirected DBLP coauthor graph

Directory: `dataset/processed/dblp_2026_10_03_coauthor`.

```json
{
  "format": "khyper.graph.v1",
  "id": "dblp-2026-10-03-coauthor",
  "name": "DBLP Coauthorship, 2026-10-03",
  "description": "One vertex per distinct author string in the prepared DBLP export. This export does not establish real-person disambiguation.",
  "directed": false,
  "vertices": {
    "path": "authors.tsv",
    "format": "tsv",
    "quoting": "none",
    "columns": {"id": "author_id", "name": "author_name"},
    "types": {"author_id": "int64", "author_name": "string"}
  },
  "edges": {
    "path": "coauthor_edges.tsv",
    "format": "tsv",
    "quoting": "none",
    "columns": {"source": "source", "target": "target", "weight": "shared_publication_count"},
    "types": {"source": "int64", "target": "int64", "shared_publication_count": "int64"},
    "weight_unit": "shared_publications",
    "weight_semantics": "Number of retained DBLP publication records coauthored by the unordered author pair, under the record-type filters documented in metadata.json."
  }
}
```

The existing metadata reports 4,174,082 author vertices and 32,569,517 unique undirected edges. The existing `coauthor_graph.sqlite` is not required by this profile and is not a supported version-1 input. The TSVs can be read directly without conversion. These sizes also imply a meaningful memory requirement; acceptance of the format is not a promise that every graph fits a given computer.

Directories named `*_author_hypergraph`, and the multi-view `openaire_2018_2026_khyper` directory, do not automatically qualify as ordinary-graph inputs. This version targets the two explicit coauthor projections above.

## 8. Validation and failure behavior

Before marking a dataset ready, a conforming loader MUST validate:

1. Descriptor version, required fields, allowed fields, JSON value types, and unambiguous column-role mappings.
2. Readability of both referenced files and compatibility of mapped columns with their declared encodings/types.
3. Non-null, unique, in-range vertex IDs; complete endpoint references.
4. Two distinct endpoints per edge and no duplicate logical edges under the declared directedness.
5. Valid weights and optional scalar properties.
6. Correct preservation of isolated vertices and external ID mappings.

Failures MUST identify the table, physical field, and offending external ID or record ordinal when available. Errors for large files may be bounded to a sample, but loading must not silently skip bad records. Record ordinals are one-based data-record positions, excluding the TSV header; they are not necessarily physical line numbers when quoted fields contain newlines.

The loader MUST NOT repair duplicate edges, remove self-loops, guess directedness, infer business meaning from filenames, or manufacture missing weights during selection. Such preparation belongs to the dataset provider. Existing permissive company-worker behavior that excludes self-loops or aggregates duplicate neighbor entries is not the validation policy of this new contract.

Header/schema checks alone do not constitute full validation. Index construction and validation may require substantial time and memory, especially for duplicate detection. The implementation must surface resource failures rather than mark an incomplete graph ready.

## 9. Dataset-neutral analysis boundary

After loading, the data service SHOULD expose at least the selected dataset ID/name, directedness, mapped property names/types, vertex/edge counts, weight unit/semantics, and applicable capabilities.

Every analysis request MUST bind to one selected dataset. The agent's instructions and tools must be refreshed for that dataset: company-specific terminology/aliases for talent flow, author/coauthor terminology for the coauthor graphs, and no directional tools on undirected graphs.

Choosing an external dataset is a read-and-load operation. Persistent analytical results, multi-round session reuse, graph mutation, and roll-up state management remain separate features and are not implied by this format.

## 10. Implementation changes required after adopting this document

The following integration work is now implemented (source datasets remain unchanged):

- Replace the fixed company paths and mandatory `.khgb` preprocessing with descriptor-driven direct Parquet/TSV loading into the existing graph storage.
- Add dataset selection and load-state handling, keeping source files outside the project.
- Generalize company-specific field assumptions and add undirected degree/strength/neighbor tools.
- Change externally visible vertex IDs and tool argument schemas from JSON numbers to decimal strings, including the browser and worker protocol boundaries.
- Apply the strict version-1 validation policy and bind agent context to the selected dataset.

Users should only need to prepare the two tables and write the small descriptor. They should not need to rename existing columns, duplicate large graph files, run a KHyper-specific converter, or supply a private binary cache.
