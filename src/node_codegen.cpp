#include "node_codegen.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace ember::node_graph {
namespace {

std::string comment_text(std::string text) {
    for (char& c : text) {
        if (c == '\r' || c == '\n' || static_cast<unsigned char>(c) < 0x20) c = ' ';
    }
    return text;
}

std::string variable(const Node& node, const std::string& suffix = {}) {
    return "node_" + node.name + suffix;
}

double numeric_parameter(const Node& node, const char* name, double fallback) {
    const auto found = node.parameters.find(name);
    if (found == node.parameters.end()) return fallback;
    if (const auto* value = std::get_if<double>(&found->second)) return *value;
    return fallback;
}

std::string string_parameter(const Node& node, const char* name, const char* fallback) {
    const auto found = node.parameters.find(name);
    if (found == node.parameters.end()) return fallback;
    if (const auto* value = std::get_if<std::string>(&found->second)) return *value;
    return fallback;
}

std::string number(double value) {
    std::ostringstream out;
    out << std::setprecision(9) << value;
    std::string result = out.str();
    if (result.find_first_of(".eE") == std::string::npos) result += ".0";
    return result;
}

std::vector<const Node*> topological_nodes(const Graph& graph) {
    std::unordered_map<std::string, std::size_t> index;
    std::vector<int> indegree(graph.nodes.size(), 0);
    std::vector<std::vector<std::size_t>> edges(graph.nodes.size());
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) index[graph.nodes[i].name] = i;
    for (const Connection& connection : graph.connections) {
        const std::size_t source = index.at(connection.source_node);
        const std::size_t destination = index.at(connection.destination_node);
        edges[source].push_back(destination);
        ++indegree[destination];
    }

    std::vector<const Node*> order;
    std::vector<bool> emitted(graph.nodes.size(), false);
    while (order.size() != graph.nodes.size()) {
        bool progressed = false;
        for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
            if (emitted[i] || indegree[i] != 0) continue;
            emitted[i] = true;
            progressed = true;
            order.push_back(&graph.nodes[i]);
            for (std::size_t destination : edges[i]) --indegree[destination];
        }
        if (!progressed) throw std::invalid_argument("cannot generate a cyclic node graph");
    }
    return order;
}

std::string input_expression(const Graph& graph, const Node& node) {
    std::vector<std::string> terms;
    for (const Connection& connection : graph.connections) {
        if (connection.destination_node == node.name)
            terms.push_back(variable(*std::find_if(graph.nodes.begin(), graph.nodes.end(),
                [&](const Node& candidate) { return candidate.name == connection.source_node; })));
    }
    if (terms.empty()) return "input";
    std::ostringstream out;
    for (std::size_t i = 0; i < terms.size(); ++i) {
        if (i) out << " + ";
        out << terms[i];
    }
    return out.str();
}

std::string zero_array(std::size_t size) {
    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < size; ++i) {
        if (i) out << ',';
        if (i % 16 == 0) out << "\n    ";
        out << "0.0";
    }
    out << "\n]";
    return out.str();
}

} // namespace

std::string generate_vst_source(const Graph& graph, const CodegenOptions& options) {
    const ValidationResult validation = validate_graph(graph);
    if (!validation.ok()) {
        std::ostringstream message;
        message << "cannot generate invalid node graph";
        for (const std::string& error : validation.errors) message << "; " << error;
        throw std::invalid_argument(message.str());
    }
    if (options.parameter_id_base < 0)
        throw std::invalid_argument("parameter_id_base must not be negative");

    const std::vector<const Node*> order = topological_nodes(graph);
    std::unordered_map<std::string, int> parameter_ids;
    int next_parameter = options.parameter_id_base;
    for (const Node& node : graph.nodes) {
        for (const auto& parameter : node.parameters) {
            if (std::holds_alternative<double>(parameter.second))
                parameter_ids[node.name + "\n" + parameter.first] = next_parameter++;
        }
    }
    auto parameter_variable = [](const Node& node, const std::string& name) {
        return variable(node, "_param_" + name);
    };

    std::map<std::string, std::size_t> delay_capacities;
    std::size_t maximum_tail = 0;
    for (const Node& node : graph.nodes) {
        if (node.type != node_types::delay) continue;
        const double configured = numeric_parameter(node, "samples", 480.0);
        const std::size_t capacity = static_cast<std::size_t>(
            std::max(1.0, std::min(48000.0, std::ceil(configured))));
        delay_capacities[node.name] = capacity;
        maximum_tail = std::max(maximum_tail, capacity);
    }

    std::ostringstream out;
    out << "// Generated by Ember VST3 Phase 9 node graph codegen.\n"
        << "// Plugin: " << comment_text(options.plugin_name) << "\n"
        << "// Numeric parameters are assigned stable VST IDs below.\n";
    for (const Node& node : graph.nodes) {
        for (const auto& parameter : node.parameters) {
            const auto* value = std::get_if<double>(&parameter.second);
            if (!value) continue;
            out << "// Parameter " << parameter_ids.at(node.name + "\n" + parameter.first)
                << ": " << node.name << '.' << parameter.first
                << " (graph default " << number(*value) << ")\n";
        }
    }
    out << '\n';

    for (const Node& node : graph.nodes) {
        if (node.type == node_types::oscillator)
            out << "global " << variable(node, "_phase") << ": f32 = 0.0;\n";
        if (node.type == node_types::filter) {
            out << "global " << variable(node, "_state_left") << ": f32 = 0.0;\n";
            out << "global " << variable(node, "_state_right") << ": f32 = 0.0;\n";
        }
        if (node.type == node_types::delay) {
            const std::size_t capacity = delay_capacities.at(node.name);
            out << "global " << variable(node, "_line_left") << ": f32[" << capacity
                << "] = " << zero_array(capacity) << ";\n";
            out << "global " << variable(node, "_line_right") << ": f32[" << capacity
                << "] = " << zero_array(capacity) << ";\n";
            out << "global " << variable(node, "_position") << ": i64 = 0;\n";
        }
    }

    out << "\n@realtime\n"
        << "fn process_f32(ctx: i64, frames: i64) -> void {\n"
        << "    let sample_rate: f32 = audio_get_sample_rate(ctx) as f32;\n"
        << "    let input_channels: i64 = audio_get_num_input_channels(ctx);\n"
        << "    let output_channels: i64 = audio_get_num_output_channels(ctx);\n";

    for (const Node& node : graph.nodes) {
        for (const auto& parameter : node.parameters) {
            if (!std::holds_alternative<double>(parameter.second)) continue;
            out << "    let mut " << parameter_variable(node, parameter.first)
                << ": f32 = audio_get_parameter(ctx, "
                << parameter_ids.at(node.name + "\n" + parameter.first) << ");\n";
        }
        if (node.type == node_types::filter) {
            const std::string cutoff = parameter_variable(node, "cutoff");
            out << "    if (" << cutoff << " < 1.0) { " << cutoff << " = 1.0; }\n"
                << "    if (" << cutoff << " > sample_rate * 0.49) { " << cutoff
                << " = sample_rate * 0.49; }\n";
        }
        if (node.type == node_types::delay) {
            const std::string samples = parameter_variable(node, "samples");
            const std::string mix = parameter_variable(node, "mix");
            const std::string feedback = parameter_variable(node, "feedback");
            const std::size_t capacity = delay_capacities.at(node.name);
            out << "    if (" << samples << " < 1.0) { " << samples << " = 1.0; }\n"
                << "    if (" << samples << " > " << capacity << ".0) { " << samples
                << " = " << capacity << ".0; }\n"
                << "    if (" << mix << " < 0.0) { " << mix << " = 0.0; }\n"
                << "    if (" << mix << " > 1.0) { " << mix << " = 1.0; }\n"
                << "    if (" << feedback << " < (0.0f - 0.99f)) { " << feedback << " = 0.0f - 0.99f; }\n"
                << "    if (" << feedback << " > 0.99f) { " << feedback << " = 0.99f; }\n";
        }
    }

    out << "\n    let mut frame: i64 = 0;\n"
        << "    while (frame < frames) {\n"
        << "        let mut ch: i64 = 0;\n"
        << "        while (ch < output_channels) {\n"
        << "            let mut input: f32 = 0.0;\n"
        << "            if (ch < input_channels) { input = audio_load_sample(ctx, ch, frame); }\n";

    for (const Node* node_pointer : order) {
        const Node& node = *node_pointer;
        const std::string value = variable(node);
        const std::string input = input_expression(graph, node);
        out << "            let mut " << value << ": f32 = 0.0;\n";
        if (node.type == node_types::oscillator) {
            const std::string phase = variable(node, "_phase");
            const std::string waveform = string_parameter(node, "waveform", "sine");
            if (waveform == "square") {
                out << "            if (" << phase << " < 0.5) { " << value
                    << " = 1.0; } else { " << value << " = 0.0f - 1.0f; }\n";
            } else if (waveform == "saw") {
                out << "            " << value << " = " << phase << " * 2.0 - 1.0;\n";
            } else {
                out << "            " << value << " = sin(" << phase
                    << " * 6.283185307179586);\n";
            }
        } else if (node.type == node_types::gain) {
            out << "            " << value << " = (" << input << ") * "
                << parameter_variable(node, "gain") << ";\n";
        } else if (node.type == node_types::mixer) {
            out << "            " << value << " = (" << input << ") * "
                << parameter_variable(node, "level") << ";\n";
        } else if (node.type == node_types::filter) {
            const std::string alpha = variable(node, "_alpha");
            const std::string low = variable(node, "_low");
            out << "            let " << alpha << ": f32 = "
                << parameter_variable(node, "cutoff") << " / ("
                << parameter_variable(node, "cutoff") << " + sample_rate);\n"
                << "            let mut " << low << ": f32 = 0.0;\n"
                << "            if (ch == 0) {\n"
                << "                " << variable(node, "_state_left") << " = "
                << variable(node, "_state_left") << " + " << alpha << " * ((" << input
                << ") - " << variable(node, "_state_left") << ");\n"
                << "                " << low << " = " << variable(node, "_state_left") << ";\n"
                << "            } else {\n"
                << "                " << variable(node, "_state_right") << " = "
                << variable(node, "_state_right") << " + " << alpha << " * ((" << input
                << ") - " << variable(node, "_state_right") << ");\n"
                << "                " << low << " = " << variable(node, "_state_right") << ";\n"
                << "            }\n";
            if (string_parameter(node, "mode", "lowpass") == "highpass")
                out << "            " << value << " = (" << input << ") - " << low << ";\n";
            else
                out << "            " << value << " = " << low << ";\n";
        } else if (node.type == node_types::delay) {
            const std::size_t capacity = delay_capacities.at(node.name);
            const std::string read = variable(node, "_read");
            const std::string delayed = variable(node, "_delayed");
            out << "            let mut " << read << ": i64 = " << variable(node, "_position")
                << " - " << parameter_variable(node, "samples") << " as i64;\n"
                << "            if (" << read << " < 0) { " << read << " = " << read
                << " + " << capacity << "; }\n"
                << "            let mut " << delayed << ": f32 = 0.0;\n"
                << "            if (ch == 0) {\n"
                << "                " << delayed << " = " << variable(node, "_line_left")
                << '[' << read << "];\n"
                << "                " << variable(node, "_line_left") << '['
                << variable(node, "_position") << "] = (" << input << ") + " << delayed
                << " * " << parameter_variable(node, "feedback") << ";\n"
                << "            } else {\n"
                << "                " << delayed << " = " << variable(node, "_line_right")
                << '[' << read << "];\n"
                << "                " << variable(node, "_line_right") << '['
                << variable(node, "_position") << "] = (" << input << ") + " << delayed
                << " * " << parameter_variable(node, "feedback") << ";\n"
                << "            }\n"
                << "            " << value << " = (" << input << ") * (1.0f - "
                << parameter_variable(node, "mix") << ") + " << delayed << " * "
                << parameter_variable(node, "mix") << ";\n";
        }
    }

    std::vector<std::string> sinks;
    for (const Node& node : graph.nodes) {
        const bool outgoing = std::any_of(graph.connections.begin(), graph.connections.end(),
            [&](const Connection& connection) { return connection.source_node == node.name; });
        if (!outgoing) sinks.push_back(variable(node));
    }
    out << "            let mut output: f32 = ";
    if (sinks.empty()) out << "input";
    for (std::size_t i = 0; i < sinks.size(); ++i) {
        if (i) out << " + ";
        out << sinks[i];
    }
    out << ";\n"
        << "            audio_store_sample(ctx, ch, frame, output);\n"
        << "            ch = ch + 1;\n"
        << "        }\n";

    for (const Node& node : graph.nodes) {
        if (node.type == node_types::oscillator) {
            const std::string phase = variable(node, "_phase");
            out << "        " << phase << " = " << phase << " + "
                << parameter_variable(node, "frequency") << " / sample_rate;\n"
                << "        if (" << phase << " >= 1.0) { " << phase << " = " << phase
                << " - 1.0; }\n";
        }
        if (node.type == node_types::delay) {
            const std::size_t capacity = delay_capacities.at(node.name);
            const std::string position = variable(node, "_position");
            out << "        " << position << " = " << position << " + 1;\n"
                << "        if (" << position << " >= " << capacity << ") { " << position
                << " = 0; }\n";
        }
    }
    out << "        frame = frame + 1;\n"
        << "    }\n"
        << "}\n\n"
        << "fn get_latency() -> i64 { return 0; }\n"
        << "fn get_tail() -> i64 { return " << maximum_tail << "; }\n";
    return out.str();
}

} // namespace ember::node_graph
