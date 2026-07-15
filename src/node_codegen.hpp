#pragma once

#include "node_graph.hpp"

#include <string>

namespace ember::node_graph {

struct CodegenOptions {
    std::string plugin_name = "Ember Node Graph";
    int parameter_id_base = 0;
};

// Compiles an acyclic graph into a self-contained .ember source string for the
// existing VST3 wrapper. Numeric node parameters become stable VST parameter
// IDs (node order, then lexicographic parameter name). String/bool parameters
// select algorithms at generation time. Throws std::invalid_argument if the
// graph cannot be compiled.
std::string generate_vst_source(const Graph& graph,
                                const CodegenOptions& options = {});

inline std::string generate_vst_source(const Graph& graph,
                                       const std::string& plugin_name) {
    CodegenOptions options;
    options.plugin_name = plugin_name;
    return generate_vst_source(graph, options);
}

// Alias for editor/export call sites.
inline std::string graph_to_ember_vst(const Graph& graph,
                                      const CodegenOptions& options = {}) {
    return generate_vst_source(graph, options);
}

} // namespace ember::node_graph
