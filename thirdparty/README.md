# Ember third-party dependencies

## ember-imgui

`imgui/` is a Git submodule from
https://github.com/ConnorBP/ember-imgui. It is a custom Dear ImGui v1.91.9b fork
shared by Ember and the fvc/prism projects. Ember's Windows build uses its
Win32 and D3D11 backends for the VST3 editor and builds these fork additions:

- `SimpleKnob` and `FovAngleKnob` custom widgets;
- the `retro_neon` neon palette and theme;
- `ToggleSwitch` and CRT overlay helpers;
- `GNeoButtonBg`, a hook for custom button/combo background rendering.

Initialize it together with the VST3 SDK:

```bash
git submodule update --init --recursive
```

## VST3 SDK

`vst3sdk/` is the Steinberg VST3 SDK submodule used by the optional
`EMBER_BUILD_VST3` target. Ember's custom editor implements `IPlugView`
directly and does not enable VSTGUI.

## AngelScript SDK subset

Fetched from https://angelcode.com/angelscript/sdk/files/angelscript_2.38.0.zip
(Zlib license). Only the build-essential subset is kept: `sdk/angelscript/include`
+ `sdk/angelscript/source` (the bytecode interpreter + compiler). Docs/samples/
add_on removed to keep the tree small; re-fetch the full SDK from the URL above
if you need them.

Used by `examples/bench_ember_vs_as.cpp` (the v0.6 benchmark harness) — ember
(native JIT) vs AngelScript (bytecode interpreter). Run `ember bench` (the
live microbenchmark CLI, ctest target `bench_ember_vs_as`) for results.
