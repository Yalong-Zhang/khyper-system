# KHyper architecture

Browser → Python HTTP service / descriptor-path preflight → C++ native dataset reader → KHyperGraph / indexes / statistics → Python agent tool dispatch → model-generated report and evidence → browser.

Python owns configuration, HTTP, the local file picker, task lifecycle, timeouts, model API calls, tool argument validation and exact-integer JSON presentation. It reads the small descriptor for immediate request validation and resolves referenced paths, but never decodes or iterates source graph rows.

C++ independently reopens and validates the descriptor, checks Parquet/TSV schemas and field values, streams source records into KHyperGraph, builds indexes and checks structural invariants. Parquet uses the Arrow/Parquet C++ libraries bundled with the pinned PyArrow installation. TSV uses a native parser supporting UTF-8, BOM, quotes, tabs and embedded newlines. Only mapped Parquet columns are decoded, in batches of up to 32,768 rows. Source datasets remain external and read-only; no persistent conversion cache is created.

The worker receives one JSON line containing `descriptor_path`. It emits bounded JSON progress messages (`phase`, `records`), then one `ready` message. After readiness, tool requests and responses continue to use JSON Lines. Individual graph rows never cross the Python/C++ boundary. Loading errors go to stderr; Python exposes a bounded error in health and clears any partial state.

The native reader is in `backend/native/src/dataset_reader.cpp`; graph storage and queries are in `worker.cpp` and the existing vendored KHyperGraph core. The Windows build uses Visual Studio 2022 MSVC x64 to match Arrow's ABI. All executables and runtime DLLs live under `.runtime/build/`.

See [dataset-selection.md](dataset-selection.md) for dataset lifecycle and [loading-performance.md](loading-performance.md) for measurements. Configuration remains in one `config.yaml`.

## Typed query layer and agent tools

`query_engine.cpp` implements schema/profile tools, typed AND filters, projections, ordering and grouped aggregation against read-only graph storage. It scans native data without transferring graph rows to Python. Ranking and neighbor tools reuse its vertex predicate compiler; metrics remain full-graph values. Undirected edge endpoints are explicitly canonicalized by ascending external ID and each edge contributes once.

`backend/tools.py` is the shared registry for function schemas, recursive argument validation, examples, usage and UI labels. `backend/skills/tool_reference.md` is generated from it and loaded into the agent context along with graph_analysis.md. Native code independently resolves actual fields, validates operand types and enforces numerical/resource bounds. The UI uses generic evidence tables for all query outputs.

The active executable is `.runtime/build/Release/khyper_worker.exe`, with runtime DLLs beside it; start.ps1 and config.yaml use this path. A legacy root-level worker may remain locked by a previously running service until that service is stopped.
