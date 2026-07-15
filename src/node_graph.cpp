#include "node_graph.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ember::node_graph {

bool Node::operator==(const Node& other) const noexcept {
    return type == other.type && name == other.name && parameters == other.parameters &&
           input_ports == other.input_ports && output_ports == other.output_ports;
}

bool Connection::operator==(const Connection& other) const noexcept {
    return source_node == other.source_node && source_port == other.source_port &&
           destination_node == other.destination_node &&
           destination_port == other.destination_port;
}

bool Graph::operator==(const Graph& other) const noexcept {
    return nodes == other.nodes && connections == other.connections;
}

bool is_builtin_node_type(const std::string& type) noexcept {
    return type == node_types::oscillator || type == node_types::filter ||
           type == node_types::gain || type == node_types::mixer ||
           type == node_types::delay;
}

Node make_builtin_node(const std::string& type, std::string name) {
    Node node;
    node.type = type;
    node.name = std::move(name);
    node.output_ports = {{"out"}};

    if (type == node_types::oscillator) {
        node.parameters = {{"frequency", 440.0}, {"waveform", std::string("sine")}};
    } else if (type == node_types::filter) {
        node.parameters = {{"cutoff", 1000.0}, {"mode", std::string("lowpass")}};
        node.input_ports = {{"in"}};
    } else if (type == node_types::gain) {
        node.parameters = {{"gain", 1.0}};
        node.input_ports = {{"in"}};
    } else if (type == node_types::mixer) {
        node.parameters = {{"level", 1.0}};
        node.input_ports = {{"in1"}, {"in2"}, {"in3"}, {"in4"}};
    } else if (type == node_types::delay) {
        node.parameters = {{"feedback", 0.35}, {"mix", 0.5}, {"samples", 1.0}};
        node.input_ports = {{"in"}};
    } else {
        throw std::invalid_argument("unknown built-in node type: " + type);
    }
    return node;
}

namespace {

bool valid_identifier(const std::string& value) {
    if (value.empty() || !(std::isalpha(static_cast<unsigned char>(value[0])) ||
                           value[0] == '_'))
        return false;
    return std::all_of(value.begin() + 1, value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_';
    });
}

bool has_port(const std::vector<Port>& ports, const std::string& name) {
    return std::any_of(ports.begin(), ports.end(), [&](const Port& port) {
        return port.name == name;
    });
}

bool string_parameter_is(const Node& node, const char* key,
                         std::initializer_list<const char*> allowed) {
    const auto found = node.parameters.find(key);
    if (found == node.parameters.end()) return true;
    const auto* value = std::get_if<std::string>(&found->second);
    if (!value) return false;
    return std::any_of(allowed.begin(), allowed.end(), [&](const char* option) {
        return *value == option;
    });
}

void append_escaped(std::ostringstream& out, const std::string& value) {
    out << '"';
    static const char hex[] = "0123456789abcdef";
    for (unsigned char c : value) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20) {
                out << "\\u00" << hex[c >> 4] << hex[c & 0x0f];
            } else {
                out << static_cast<char>(c);
            }
        }
    }
    out << '"';
}

void append_ports(std::ostringstream& out, const std::vector<Port>& ports) {
    out << '[';
    for (std::size_t i = 0; i < ports.size(); ++i) {
        if (i) out << ',';
        append_escaped(out, ports[i].name);
    }
    out << ']';
}

void append_parameter(std::ostringstream& out, const ParameterValue& value) {
    if (const auto* number = std::get_if<double>(&value)) {
        if (!std::isfinite(*number))
            throw std::invalid_argument("JSON cannot serialize a non-finite parameter");
        out << std::setprecision(std::numeric_limits<double>::max_digits10) << *number;
    } else if (const auto* text = std::get_if<std::string>(&value)) {
        append_escaped(out, *text);
    } else {
        out << (std::get<bool>(value) ? "true" : "false");
    }
}

// Purpose-built strict JSON parser for the graph schema. It accepts ordinary
// JSON whitespace and object member order, but rejects unknown/duplicate
// fields so corrupted editor files cannot silently change meaning.
class JsonParser {
public:
    explicit JsonParser(const std::string& input) : input_(input) {}

    Graph parse_graph_document() {
        Graph graph;
        bool have_nodes = false;
        bool have_connections = false;
        object([&](const std::string& key) {
            if (key == "nodes") {
                duplicate(have_nodes, key);
                graph.nodes = array<Node>([&] { return parse_node(); });
            } else if (key == "connections") {
                duplicate(have_connections, key);
                graph.connections = array<Connection>([&] { return parse_connection(); });
            } else {
                fail("unknown graph field '" + key + "'");
            }
        });
        if (!have_nodes || !have_connections)
            fail("graph requires nodes and connections");
        space();
        if (position_ != input_.size()) fail("trailing characters");
        return graph;
    }

private:
    const std::string& input_;
    std::size_t position_ = 0;

    [[noreturn]] void fail(const std::string& message) const {
        throw std::invalid_argument("invalid graph JSON at byte " +
                                    std::to_string(position_) + ": " + message);
    }

    void space() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_])))
            ++position_;
    }

    bool take(char expected) {
        space();
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void require(char expected) {
        if (!take(expected)) fail(std::string("expected '") + expected + "'");
    }

    static void duplicate(bool& seen, const std::string& key) {
        if (seen) throw std::invalid_argument("duplicate JSON field: " + key);
        seen = true;
    }

    template <typename Visitor>
    void object(Visitor visit) {
        require('{');
        if (take('}')) return;
        do {
            std::string key = string();
            require(':');
            visit(key);
        } while (take(','));
        require('}');
    }

    template <typename T, typename Reader>
    std::vector<T> array(Reader read) {
        std::vector<T> values;
        require('[');
        if (take(']')) return values;
        do {
            values.push_back(read());
        } while (take(','));
        require(']');
        return values;
    }

    std::string string() {
        space();
        if (position_ >= input_.size() || input_[position_++] != '"')
            fail("expected string");
        std::string result;
        while (position_ < input_.size()) {
            const unsigned char c = static_cast<unsigned char>(input_[position_++]);
            if (c == '"') return result;
            if (c < 0x20) fail("control character in string");
            if (c != '\\') {
                result.push_back(static_cast<char>(c));
                continue;
            }
            if (position_ >= input_.size()) fail("unfinished escape");
            const char escape = input_[position_++];
            switch (escape) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                unsigned value = 0;
                for (int i = 0; i < 4; ++i) {
                    if (position_ >= input_.size()) fail("unfinished unicode escape");
                    const char h = input_[position_++];
                    value <<= 4;
                    if (h >= '0' && h <= '9') value += static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') value += static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') value += static_cast<unsigned>(h - 'A' + 10);
                    else fail("bad unicode escape");
                }
                if (value >= 0xd800 && value <= 0xdfff)
                    fail("surrogate escapes are not supported");
                if (value <= 0x7f) result.push_back(static_cast<char>(value));
                else if (value <= 0x7ff) {
                    result.push_back(static_cast<char>(0xc0 | (value >> 6)));
                    result.push_back(static_cast<char>(0x80 | (value & 0x3f)));
                } else {
                    result.push_back(static_cast<char>(0xe0 | (value >> 12)));
                    result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
                    result.push_back(static_cast<char>(0x80 | (value & 0x3f)));
                }
                break;
            }
            default: fail("bad escape");
            }
        }
        fail("unterminated string");
    }

    double number() {
        space();
        const std::size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        if (position_ >= input_.size()) fail("expected number");
        if (input_[position_] == '0') {
            ++position_;
        } else if (input_[position_] >= '1' && input_[position_] <= '9') {
            while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_])))
                ++position_;
        } else {
            fail("expected number");
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            const std::size_t digits = position_;
            while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_])))
                ++position_;
            if (digits == position_) fail("expected fractional digits");
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            const std::size_t digits = position_;
            while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_])))
                ++position_;
            if (digits == position_) fail("expected exponent digits");
        }
        const std::string token = input_.substr(start, position_ - start);
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(value)) fail("invalid number");
        return value;
    }

    bool literal(const char* token, bool value) {
        space();
        const std::size_t length = std::char_traits<char>::length(token);
        if (input_.compare(position_, length, token) != 0) fail("expected JSON value");
        position_ += length;
        return value;
    }

    ParameterValue parameter() {
        space();
        if (position_ >= input_.size()) fail("expected parameter value");
        if (input_[position_] == '"') return string();
        if (input_[position_] == 't') return literal("true", true);
        if (input_[position_] == 'f') return literal("false", false);
        return number();
    }

    std::vector<Port> ports() {
        const auto names = array<std::string>([&] { return string(); });
        std::vector<Port> result;
        result.reserve(names.size());
        for (const auto& name : names) result.push_back({name});
        return result;
    }

    ParameterMap parameters() {
        ParameterMap result;
        object([&](const std::string& key) {
            if (!result.emplace(key, parameter()).second)
                fail("duplicate parameter '" + key + "'");
        });
        return result;
    }

    Node parse_node() {
        Node node;
        bool type = false, name = false, params = false, inputs = false, outputs = false;
        object([&](const std::string& key) {
            if (key == "type") { duplicate(type, key); node.type = string(); }
            else if (key == "name") { duplicate(name, key); node.name = string(); }
            else if (key == "parameters") { duplicate(params, key); node.parameters = parameters(); }
            else if (key == "inputs") { duplicate(inputs, key); node.input_ports = ports(); }
            else if (key == "outputs") { duplicate(outputs, key); node.output_ports = ports(); }
            else fail("unknown node field '" + key + "'");
        });
        if (!type || !name || !params || !inputs || !outputs)
            fail("node requires type, name, parameters, inputs, and outputs");
        return node;
    }

    Connection parse_connection() {
        Connection connection;
        bool sn = false, sp = false, dn = false, dp = false;
        object([&](const std::string& key) {
            if (key == "source_node") { duplicate(sn, key); connection.source_node = string(); }
            else if (key == "source_port") { duplicate(sp, key); connection.source_port = string(); }
            else if (key == "destination_node") { duplicate(dn, key); connection.destination_node = string(); }
            else if (key == "destination_port") { duplicate(dp, key); connection.destination_port = string(); }
            else fail("unknown connection field '" + key + "'");
        });
        if (!sn || !sp || !dn || !dp) fail("connection is missing an endpoint field");
        return connection;
    }
};

} // namespace

ValidationResult validate_graph(const Graph& graph) {
    ValidationResult result;
    std::unordered_map<std::string, const Node*> nodes;

    for (const Node& node : graph.nodes) {
        if (!is_builtin_node_type(node.type))
            result.errors.push_back("unknown node type: " + node.type);
        if (!valid_identifier(node.name))
            result.errors.push_back("invalid node name: " + node.name);
        if (!nodes.emplace(node.name, &node).second)
            result.errors.push_back("duplicate node name: " + node.name);
        if (node.output_ports.empty())
            result.errors.push_back("node has no output port: " + node.name);

        std::set<std::string> port_names;
        for (const Port& port : node.input_ports) {
            if (port.name.empty() || !port_names.insert("i:" + port.name).second)
                result.errors.push_back("invalid or duplicate input port on " + node.name);
        }
        for (const Port& port : node.output_ports) {
            if (port.name.empty() || !port_names.insert("o:" + port.name).second)
                result.errors.push_back("invalid or duplicate output port on " + node.name);
        }

        if (node.type == node_types::oscillator &&
            !string_parameter_is(node, "waveform", {"sine", "square", "saw"}))
            result.errors.push_back("oscillator waveform must be sine, square, or saw: " + node.name);
        if (node.type == node_types::filter &&
            !string_parameter_is(node, "mode", {"lowpass", "highpass"}))
            result.errors.push_back("filter mode must be lowpass or highpass: " + node.name);
        for (const auto& parameter : node.parameters) {
            if (const auto* value = std::get_if<double>(&parameter.second); value && !std::isfinite(*value))
                result.errors.push_back("non-finite parameter on " + node.name + ": " + parameter.first);
        }
    }

    std::set<std::string> connections;
    std::unordered_map<std::string, std::vector<std::string>> edges;
    std::unordered_map<std::string, int> indegree;
    for (const Node& node : graph.nodes) indegree[node.name] = 0;

    for (const Connection& connection : graph.connections) {
        const auto source = nodes.find(connection.source_node);
        const auto destination = nodes.find(connection.destination_node);
        if (source == nodes.end()) {
            result.errors.push_back("missing source node: " + connection.source_node);
        } else if (!has_port(source->second->output_ports, connection.source_port)) {
            result.errors.push_back("missing source port: " + connection.source_node + "." + connection.source_port);
        }
        if (destination == nodes.end()) {
            result.errors.push_back("missing destination node: " + connection.destination_node);
        } else if (!has_port(destination->second->input_ports, connection.destination_port)) {
            result.errors.push_back("missing destination port: " + connection.destination_node + "." + connection.destination_port);
        }
        const std::string key = connection.source_node + "\n" + connection.source_port + "\n" +
                                connection.destination_node + "\n" + connection.destination_port;
        if (!connections.insert(key).second) result.errors.push_back("duplicate connection");
        if (source != nodes.end() && destination != nodes.end()) {
            edges[connection.source_node].push_back(connection.destination_node);
            ++indegree[connection.destination_node];
        }
    }

    std::vector<std::string> ready;
    for (const auto& entry : indegree) if (entry.second == 0) ready.push_back(entry.first);
    std::size_t visited = 0;
    while (!ready.empty()) {
        const std::string current = ready.back();
        ready.pop_back();
        ++visited;
        for (const std::string& next : edges[current])
            if (--indegree[next] == 0) ready.push_back(next);
    }
    if (visited != graph.nodes.size()) result.errors.push_back("graph contains a directed cycle");

    return result;
}

std::string to_json(const Graph& graph) {
    std::ostringstream out;
    out << "{\"nodes\":[";
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        if (i) out << ',';
        const Node& node = graph.nodes[i];
        out << "{\"type\":";
        append_escaped(out, node.type);
        out << ",\"name\":";
        append_escaped(out, node.name);
        out << ",\"parameters\":{";
        std::size_t parameter_index = 0;
        for (const auto& parameter : node.parameters) {
            if (parameter_index++) out << ',';
            append_escaped(out, parameter.first);
            out << ':';
            append_parameter(out, parameter.second);
        }
        out << "},\"inputs\":";
        append_ports(out, node.input_ports);
        out << ",\"outputs\":";
        append_ports(out, node.output_ports);
        out << '}';
    }
    out << "],\"connections\":[";
    for (std::size_t i = 0; i < graph.connections.size(); ++i) {
        if (i) out << ',';
        const Connection& connection = graph.connections[i];
        out << "{\"source_node\":";
        append_escaped(out, connection.source_node);
        out << ",\"source_port\":";
        append_escaped(out, connection.source_port);
        out << ",\"destination_node\":";
        append_escaped(out, connection.destination_node);
        out << ",\"destination_port\":";
        append_escaped(out, connection.destination_port);
        out << '}';
    }
    out << "]}";
    return out.str();
}

Graph graph_from_json(const std::string& json) {
    return JsonParser(json).parse_graph_document();
}

std::string Graph::to_json() const {
    return node_graph::to_json(*this);
}

Graph Graph::from_json(const std::string& json) {
    return graph_from_json(json);
}

} // namespace ember::node_graph
