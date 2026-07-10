# ember thirdparty — AngelScript SDK (subset)

Fetched from https://angelcode.com/angelscript/sdk/files/angelscript_2.38.0.zip
(Zlib license). Only the build-essential subset is kept: `sdk/angelscript/include`
+ `sdk/angelscript/source` (the bytecode interpreter + compiler). Docs/samples/
add_on removed to keep the tree small; re-fetch the full SDK from the URL above
if you need them.

Used by `examples/bench_ember_vs_as.cpp` (the v0.6 benchmark harness) — ember
(native JIT) vs AngelScript (bytecode interpreter). Run `ember bench` (the
live microbenchmark CLI, ctest target `bench_ember_vs_as`) for results.
