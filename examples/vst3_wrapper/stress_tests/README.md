# VST3 validation and stress suite

`vst3_stress_tests` is built when `EMBER_BUILD_VST3=ON` and the pinned SDK
submodule is available. The root CMake file registers four CTest modes.

| CTest | Direct mode | Coverage |
|---|---|---|
| `vst3_stress` | `stress` | 32/44.1/48/96/192 kHz; 0, 1 and 16-8192-frame blocks; changing block sizes; silence; mono/stereo/4/8-channel `ProcessData` shapes; 128 parameter queues; 1200 events; hot reload while audio processing runs |
| `vst3_realtime_contract` | `realtime` | warmed `process()` calls at 1-8192 frames; zero global C++ allocations; unchanged GC collection count; lock-free wrapper path; each measured call below 50% of its block deadline |
| `vst3_fuzz` | `fuzz` | 5000 deterministic random process shapes with 1-8 channels, 0-8192 frames, automation, event lists, and optional input |
| `vst3_soak` | `soak --seconds N` | paced continuous processing with CPU and resident-memory growth reporting; CTest uses 30 seconds, direct mode defaults to 60 |

Configure and build from the repository root:

```bash
cmake -S . -B buildt -G Ninja \
  -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe \
  -DEMBER_BUILD_VST3=ON
cmake --build buildt -j 8
```

Run through CTest:

```bash
ctest --test-dir buildt -R '^vst3_(stress|realtime_contract|fuzz)$' --output-on-failure
ctest --test-dir buildt -R '^vst3_soak$' --output-on-failure
```

Or invoke one mode directly (the executable location is generator-dependent):

```bash
buildt/examples/vst3_wrapper/stress_tests/vst3_stress_tests.exe fuzz
buildt/examples/vst3_wrapper/stress_tests/vst3_stress_tests.exe soak --seconds 60
```

`vst3_soak` is labelled `soak`; a short normal gate can exclude it with
`ctest --test-dir buildt -LE soak -E bench`.

## Steinberg validator checkpoint

On 2026-07-12, the Release `ember_gain.vst3` bundle passed the pinned VST3 SDK
3.8.0 validator: **47 passed, 0 failed**. The run covered 32/64-bit processing,
threaded processing, variable blocks, parameter flushes and automation,
silence, state transitions, bus consistency, and the validator's sample-rate
matrix.

```bash
cmake --build buildt --target validator -j 8
buildt/bin/validator.exe buildt/VST3/Release/ember_gain.vst3
```

MinGW needs `-municode` for the SDK validator's `wmain`; the wrapper CMake adds
that option to the validator target only.
