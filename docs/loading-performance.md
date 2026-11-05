# Native dataset loading performance

Measured on the user's Windows 11 machine, 2026-11-04. The current loader reads
external Parquet/TSV files directly in C++; Python sends only the descriptor path.
No source dataset is copied, rewritten or converted to an on-disk cache.

## Current version comparison

Both versions were measured in this session, twice per dataset, with a new worker
each run. Wall time covers Python descriptor/path preflight, process startup,
source reading, schema/value validation, graph construction, indexing and structural
validation until ready. Query checks run after the timer stops.

| Descriptor | Vertices | Edges | Python stream, seconds | Native read, seconds | Mean time reduction |
| --- | ---: | ---: | --- | --- | ---: |
| `openaire-coauthor.json` | 198,393 | 330,373 | 3.249 / 3.202 (3.226) | 1.354 / 1.331 (1.343) | 58.4% |
| `directed-example.json` | 788,007 | 2,210,046 | 13.849 / 13.421 (13.635) | 7.829 / 7.150 (7.490) | 45.1% |

These are repeated local loads; OS file caching is uncontrolled, and they are not
cold-disk benchmarks. The native version uses MSVC Release to match the ABI of the
Arrow libraries, whereas the previous worker used GCC Release. The comparison
therefore measures the complete version change, including the necessary compiler
switch; it does not isolate individual optimizations. No large TSV speedup is
claimed: the full 32.6-million-edge DBLP dataset was not benchmarked.

Both versions produce identical vertex/edge counts and total weights (1,331,715
and 2,628,805). For each dataset/run, weighted and unweighted top-10 rankings, and
weighted/unweighted statistics and neighbor lists for each ranking's top vertex,
were compared exactly. These are sampled query checks, not an exhaustive comparison
of every possible query. Local raw timings and query snapshots are under
`.runtime/native-before.json` and `.runtime/native-after.json`; they are temporary.

## Why loading is faster

Previously, PyArrow decoded Parquet in native code, but Python then converted
columns to lists and row objects, packed records and streamed every row through
stdin. C++ unpacked structural fields and parsed node/property JSON before building
the graph. That entire row-transfer protocol and Python source reader are removed.

Now `backend/native/src/dataset_reader.cpp`:

- Independently validates the descriptor, source schemas, scalar values and types.
- Projects mapped Parquet columns using Arrow's schema manifest (including correct
  leaf indexes when ignored nested fields exist), and streams 32,768-row batches.
- Parses quoted UTF-8 TSV records natively, including BOM, CRLF and embedded lines.
- Passes in-memory property records directly into the graph builder, with no
  per-row JSON serialization or inter-process transfer.
- Preserves external int64 IDs, nullable scalar properties, unit-weight semantics,
  isolated vertices, source-change checks and overflow/graph validation.
- Emits progress messages to Python; error strings with record/field context are
  constructed only when a record fails validation.

Python retains HTTP, configuration, path/descriptor preflight, task lifecycle,
model API calls and tool dispatch. The existing KHyperGraph core and public query
semantics are retained. `neusight-exp` and all external source files are unchanged.

## Build and verification

Windows requires Visual Studio 2022 Desktop development with C++ (MSVC x64 and a
Windows SDK), CMake 3.20+ and the pinned PyArrow installation in the project venv.
`setup.ps1` discovers Arrow/Parquet headers and import libraries from PyArrow, builds
with MSVC, and copies Arrow/Parquet plus wheel-bundled MSVC runtime DLLs into
`.runtime/build/Release/`. Python does not import PyArrow on the application load path;
PyArrow remains a build dependency and creates fixtures in tests.

At the time of the native-reader migration, 43 Python tests and the C++ incidence test passed. The later query-tools release adds further coverage; see query-tools.md. Coverage includes directed and
undirected graphs, both formats, quoted TSV, Unicode paths/text, scalar properties,
batch boundaries, ignored nested columns, empty graphs, int64 boundaries, malformed
values/schemas/UTF-8, invalid graph structure and asynchronous HTTP failure state.
No model API calls were made during this migration.

From the project directory:

```powershell
.\setup.ps1 -SkipInstall
.\.venv\Scripts\python.exe -m pytest -q
ctest --test-dir .runtime/build --output-on-failure -C Release
.\.venv\Scripts\python.exe backend/maintenance/benchmark_loading.py examples/datasets/openaire-coauthor.json examples/datasets/directed-example.json --repeats 2 --output .runtime/load-after.json
```

Restart the service with `start.ps1`, then select a dataset again to use the new
loader and executable. The current executable has already been built and tested.

## Earlier optimization history

Before native reading, the original Python-streaming implementation took about
5.18 seconds for OpenAIRE and 28.14 seconds for the directed example. Column-wise
Arrow checks, binary structural edges, capacity hints and a lighter native incidence
validator reduced those earlier measurements to 2.97 and 12.07 seconds. Those runs
were made separately from the current comparison; use the table above for this
migration's measured improvement.
