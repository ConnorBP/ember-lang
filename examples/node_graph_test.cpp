// VST3 Phase 9 node graph regression tests.
// Written before the implementation: this pins the public model, JSON
// round-trip, validation, deterministic generation, and wrapper API surface.
#include "node_codegen.hpp"
#include "node_graph.hpp"

#include "binding_builder.hpp"
#include "ext_audio.hpp"
#include "ext_math.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace ng = ember::node_graph;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}

ng::Graph example_graph() {
    ng::Graph graph;

    ng::Node oscillator = ng::make_builtin_node(ng::node_types::oscillator, "lead");
    oscillator.parameters["waveform"] = std::string("sine");
    oscillator.parameters["frequency"] = 220.0;

    ng::Node filter = ng::make_builtin_node(ng::node_types::filter, "tone");
    filter.parameters["mode"] = std::string("lowpass");
    filter.parameters["cutoff"] = 2400.0;

    ng::Node gain = ng::make_builtin_node(ng::node_types::gain, "level");
    gain.parameters["gain"] = 0.25;

    graph.nodes = {oscillator, filter, gain};
    graph.connections = {
        {"lead", "out", "tone", "in"},
        {"tone", "out", "level", "in"},
    };
    return graph;
}

bool semantically_valid_source(const std::string& source) {
    auto lexed = ember::tokenize(source, "generated_node_graph.ember");
    if (!lexed.ok) return false;
    auto parsed = ember::parse(std::move(lexed.toks));
    if (!parsed.ok) return false;

    std::unordered_map<std::string, ember::NativeSig> natives;
    ember::ext_audio::register_natives(natives);
    ember::ext_math::register_natives(natives);
    std::unordered_map<std::string, int> slots;
    for (std::size_t i = 0; i < parsed.program.funcs.size(); ++i) {
        parsed.program.funcs[i].slot = static_cast<int>(i);
        slots[parsed.program.funcs[i].name] = static_cast<int>(i);
    }
    auto layouts = ember::build_struct_layouts(parsed.program);
    const auto checked = ember::sema(parsed.program, natives, slots,
                                     ember::PERM_FFI, nullptr, &layouts);
    if (!checked.ok) {
        for (const auto& error : checked.errors)
            std::printf("  generated sema error %d:%d: %s\n",
                        error.line, error.col, error.msg.c_str());
    }
    return checked.ok;
}

ng::Graph every_node_graph() {
    ng::Graph graph;
    auto square = ng::make_builtin_node(ng::node_types::oscillator, "square_source");
    square.parameters["waveform"] = std::string("square");
    auto saw = ng::make_builtin_node(ng::node_types::oscillator, "saw_source");
    saw.parameters["waveform"] = std::string("saw");
    auto mixer = ng::make_builtin_node(ng::node_types::mixer, "mix");
    auto delay = ng::make_builtin_node(ng::node_types::delay, "echo");
    delay.parameters["samples"] = 4.0;
    auto highpass = ng::make_builtin_node(ng::node_types::filter, "high");
    highpass.parameters["mode"] = std::string("highpass");
    graph.nodes = {square, saw, mixer, delay, highpass};
    graph.connections = {
        {"square_source", "out", "mix", "in1"},
        {"saw_source", "out", "mix", "in2"},
        {"mix", "out", "echo", "in"},
        {"echo", "out", "high", "in"},
    };
    return graph;
}

} // namespace

int main() {
    const ng::Graph graph = example_graph();
    check(graph.nodes.size() == 3, "graph stores nodes");
    check(graph.connections.size() == 2, "graph stores connections");
    check(ng::is_builtin_node_type("oscillator"), "oscillator is built in");
    check(ng::is_builtin_node_type("mixer"), "mixer is built in");
    check(!ng::is_builtin_node_type("mystery"), "unknown node is not built in");

    const std::string json = ng::to_json(graph);
    const ng::Graph loaded = ng::graph_from_json(json);
    check(loaded == graph, "JSON graph round-trips exactly");
    check(ng::to_json(loaded) == json, "JSON serialization is deterministic");
    check(std::get<std::string>(loaded.nodes[0].parameters.at("waveform")) == "sine",
          "JSON preserves string parameters");
    check(std::get<double>(loaded.nodes[2].parameters.at("gain")) == 0.25,
          "JSON preserves numeric parameters");

    bool malformed_rejected = false;
    try {
        (void)ng::graph_from_json("{\"nodes\":[}");
    } catch (const std::invalid_argument&) {
        malformed_rejected = true;
    }
    check(malformed_rejected, "malformed JSON is rejected");

    const auto validation = ng::validate_graph(graph);
    check(validation.ok(), "connected built-in graph validates");

    ng::Graph broken = graph;
    broken.connections.push_back({"missing", "out", "level", "in"});
    check(!ng::validate_graph(broken).ok(), "connection to missing node is rejected");

    const std::string source = ng::generate_vst_source(graph, "Generated Lead");
    check(source.find("fn process_f32(ctx: i64, frames: i64) -> void") != std::string::npos,
          "generator emits VST process callback");
    check(source.find("audio_load_sample(ctx, ch, frame)") != std::string::npos,
          "generator uses wrapper input buffers");
    check(source.find("audio_store_sample(ctx, ch, frame") != std::string::npos,
          "generator uses wrapper output buffers");
    check(source.find("audio_get_parameter(ctx, 0)") != std::string::npos,
          "generator exposes graph values as VST parameters");
    check(source.find("sin(") != std::string::npos,
          "generator emits oscillator DSP");
    check(source.find("node_tone") != std::string::npos,
          "generator emits filter DSP");
    check(source.find("fn get_latency() -> i64") != std::string::npos,
          "generator emits wrapper metadata callbacks");
    check(source.find("Generated Lead") != std::string::npos,
          "generator includes plugin name");
    check(ng::generate_vst_source(graph, "Generated Lead") == source,
          "code generation is deterministic");

    check(semantically_valid_source(source),
          "generated source passes VST wrapper syntax and semantic checks");

    const std::string all_nodes_source = ng::generate_vst_source(every_node_graph());
    check(all_nodes_source.find("node_echo_line_left") != std::string::npos,
          "generator emits delay state");
    check(all_nodes_source.find("node_mix") != std::string::npos,
          "generator emits mixer DSP");
    check(all_nodes_source.find("node_high_low") != std::string::npos,
          "generator emits highpass DSP");
    check(semantically_valid_source(all_nodes_source),
          "all built-in node types generate valid ember source");

    bool cycle_rejected = false;
    ng::Graph cyclic = graph;
    cyclic.connections.push_back({"level", "out", "tone", "in"});
    try {
        (void)ng::generate_vst_source(cyclic);
    } catch (const std::invalid_argument&) {
        cycle_rejected = true;
    }
    check(cycle_rejected, "generator rejects cyclic graphs");

    std::printf("node graph tests: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
