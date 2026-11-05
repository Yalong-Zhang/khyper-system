# Vendored dependencies

`khypergraph/` contains a snapshot of these files from the user's local KHyper research repository (`neusight-exp`):

- `include/khypergraph/hyper_graph.hpp`
- `include/khypergraph/vertex.hpp`
- `include/khypergraph/edge.hpp`
- `include/khypergraph/types.hpp`
- `src/hyper_graph.cpp`

Only the existing graph storage core is needed in this first version. The source repository is not a runtime dependency and was not edited. This local copy optimizes `HyperGraph::validate()` by checking append-ordered incidence lists with per-vertex cursors instead of allocating per-vertex hash sets. Endpoint, role, lineage and incidence consistency checks remain in place; storage and public interfaces are unchanged.

`nlohmann/json.hpp` is JSON for Modern C++ 3.12.0 by Niels Lohmann, under the MIT license. It was copied from the existing local HADA/GraphCube dependency and retains its SPDX notices. See `nlohmann/LICENSE.MIT`.

Dataset descriptors reference external source files. Source graph tables are not bundled or copied into this project.
