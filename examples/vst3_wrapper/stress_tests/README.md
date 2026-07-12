# VST3 Phase 7 validation and stress suite

## Automated tests

The suite builds as `vst3_stress_tests` and is registered with CTest:

- `vst3_stress`: 32/44.1/48/96/192 kHz; 0, 1, 16..8192 sample blocks;
  changing block sizes; silence; mono/stereo/4/8-channel shapes; 128 parameter
  queues; 1200 note events; and script hot reload while an audio thread runs.
- `vst3_realtime_contract`: overrides global `new`/`new[]` and asserts zero C++
  allocations during `process()`, checks that the test GC collection count is
  unchanged, exercises the wrapper's lock-free process path, and checks each
  measured block against 50% of its realtime deadline.
- `vst3_fuzz`: 5000 deterministic random ProcessData shapes with random frame
  counts, 1-8 channels, automation, event lists, and optional input busses.
- `vst3_soak`: 60 seconds of paced continuous processing with CPU and resident
  memory-growth reporting. It is labelled `soak`, so the normal baseline command
  can skip it with `ctest -E bench -LE soak` when a short gate is required.

Run one mode directly, for example:

```sh
buildt/examples/vst3_wrapper/stress_tests/vst3_stress_tests.exe fuzz
buildt/examples/vst3_wrapper/stress_tests/vst3_stress_tests.exe soak --seconds 60
```

## Steinberg VST3 Validator result

Tested on 2026-07-12 with the pinned VST3 SDK 3.8.0 validator and the Release
`buildt/VST3/Release/ember_gain.vst3` bundle:

```sh
cmake --build buildt --target validator -j 8
buildt/bin/validator.exe buildt/VST3/Release/ember_gain.vst3
```

Result: **47 tests passed, 0 tests failed**. Both 32-bit and 64-bit suites,
threaded processing, variable blocks, parameter flushes, automation accuracy,
silence, state transitions, bus consistency, and all validator sample rates
passed. MinGW requires `-municode` for the SDK validator's `wmain`; the wrapper
CMake applies that target-local linker option without modifying the SDK.
