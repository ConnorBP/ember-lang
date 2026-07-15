#pragma once

#include <map>
#include <string>
#include <variant>
#include <vector>

namespace ember::node_graph {

// Parameters deliberately use a small JSON-compatible value set. std::map
// gives graph files and generated source stable ordering across platforms.
using ParameterValue = std::variant<double, std::string, bool>;
using ParameterMap = std::map<std::string, ParameterValue>;

struct Port {
    std::string name;

    bool operator==(const Port& other) const noexcept { return name == other.name; }
};

struct Node {
    std::string type;
    std::string name;
    ParameterMap parameters;
    std::vector<Port> input_ports;
    std::vector<Port> output_ports;

    bool operator==(const Node& other) const noexcept;
};

struct Connection {
    std::string source_node;
    std::string source_port;
    std::string destination_node;
    std::string destination_port;

    bool operator==(const Connection& other) const noexcept;
};

struct Graph {
    std::vector<Node> nodes;
    std::vector<Connection> connections;

    bool operator==(const Graph& other) const noexcept;
    std::string to_json() const;
    static Graph from_json(const std::string& json);
};

namespace node_types {
inline constexpr const char* oscillator = "oscillator";
inline constexpr const char* filter = "filter";
inline constexpr const char* gain = "gain";
inline constexpr const char* mixer = "mixer";
inline constexpr const char* delay = "delay";
} // namespace node_types

bool is_builtin_node_type(const std::string& type) noexcept;
Node make_builtin_node(const std::string& type, std::string name);

struct ValidationResult {
    std::vector<std::string> errors;
    bool ok() const noexcept { return errors.empty(); }
};

// Checks identifiers, built-in variants, endpoint existence, port names,
// duplicate names/connections, and directed cycles.
ValidationResult validate_graph(const Graph& graph);

// Throws std::invalid_argument when the input is malformed or violates the
// JSON graph schema. Semantic graph validation is deliberately separate so an
// editor can save and reopen a temporarily incomplete graph. Serialization
// emits canonical, deterministic JSON.
std::string to_json(const Graph& graph);
Graph graph_from_json(const std::string& json);

// Convenient spelling for callers that prefer save/load terminology.
inline std::string save_graph_json(const Graph& graph) { return to_json(graph); }
inline Graph load_graph_json(const std::string& json) { return graph_from_json(json); }

} // namespace ember::node_graph
