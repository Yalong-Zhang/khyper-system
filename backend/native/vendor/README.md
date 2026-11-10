# Vendored dependencies

`khypergraph/` contains a snapshot of these files from the user's local KHyper research repository (`neusight-exp`):

- `include/khypergraph/hyper_graph.hpp`
- `include/khypergraph/vertex.hpp`
- `include/khypergraph/edge.hpp`
- `include/khypergraph/types.hpp`
- `src/hyper_graph.cpp`

Only the existing graph storage core is needed in this first version. The source repository is not a runtime dependency and was not edited. This local copy optimizes `HyperGraph::validate()` by checking append-ordered incidence lists with per-vertex cursors instead of allocating per-vertex hash sets. Endpoint, role, lineage and incidence consistency checks remain in place.

`PropertyMap` uses contiguous typed key/value pairs with linear lookup for the small number of fields on each record. This avoids a separate hash bucket array and hash-node allocations per edge. It retains the map operations used by this project, including lookup, insertion, updates, erasure, iteration and independent copies. Structural endpoint fields are removed and the remaining edge properties are compacted during loading. The `HyperGraph` vertex, edge, endpoint and incidence model is retained.

`nlohmann/json.hpp` is JSON for Modern C++ 3.12.0 by Niels Lohmann, under the MIT license. It was copied from the existing local HADA/GraphCube dependency and retains its SPDX notices. See `nlohmann/LICENSE.MIT`.

Dataset descriptors reference external source files. Source graph tables are not bundled or copied into this project.
