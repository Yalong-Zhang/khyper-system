# Dataset selection implementation

The application starts with no selected graph. Use **Browse path** to open the local operating-system file picker, or enter an absolute descriptor path. Select a `khyper.graph.v1` JSON descriptor and press **Load dataset**.

The descriptor maps an external Parquet/TSV vertex table and edge table. Relative paths resolve against the descriptor directory. No table is uploaded, copied to the project, rewritten, or converted to an on-disk intermediate file. The C++ worker directly decodes Parquet with Arrow/Parquet C++ and TSV with a native quoted-field parser, feeding the existing KHyperGraph storage. Python sends only the descriptor path, not graph rows. Source schema and record errors are reported asynchronously through health after the load request is accepted.

The API accepts a load request and returns immediately. `/api/health` reports `empty`, `loading`, `ready`, or `error`, plus the current load phase and record count. Structural validation completes before `ready`. The old graph is released to bound memory use; a failed replacement leaves no active graph. Selection is blocked during another load or analysis. Analysis requests can carry a dataset ID to reject stale browser submissions.

Directed datasets expose incoming/outgoing degree and strength metrics. Undirected datasets expose degree, strength, and undirected neighbors, without directional tool metrics. External IDs remain decimal strings in all tool interfaces; integer results above JavaScript's exact range are also serialized as strings.

Source self-loops, duplicate directed pairs, reverse duplicates in undirected graphs, missing endpoints, invalid types, and invalid weights cause failure. Isolated vertices remain present. The importer never silently repairs these cases.

The descriptor examples under `examples/datasets` reference existing external data. The DBLP TSV profile is supported, but its 32.6 million edges have a substantial memory/load-time cost; no full-size DBLP run is claimed by the current validation. Small TSV graphs and full OpenAIRE/directed examples were tested.

The native picker runs on the backend computer. This matches the current local Windows application. Remote deployments would require a server-side accessible-path picker, not a browser upload disguised as a path selector.

Validation now includes native schema, quoted TSV, Unicode paths, null properties, batch boundaries, int64 limits and HTTP failure-state tests. Both real external graph examples were loaded and sampled tool results were compared with the former Python-streaming version. No model API calls are needed for these checks. See loading-performance.md for the current test count and measurements. start.ps1 launches the normal local application.
