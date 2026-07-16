# Ember script examples

Run commands from the repository root. Most scripts use extension natives, so
pass `--ffi`:

```console
build/ember_cli.exe run examples/scripts/text_formatter.ember --ffi
```

A nonzero process result is often the documented demonstration value, not a
failure; each standalone file declares `// expect: N` in its header.

## Standalone examples

| Example | Practical focus |
|---|---|
| `control.ember` | loops, switch, break/continue, defer |
| `fib.ember` | recursion |
| `string.ember`, `struct.ember` | basic extension and struct use |
| `io_test.ember`, `read_line_test.ember` | console, files, paths, stdin |
| `game_state_manager.ember` | game flow with structs, enum, match |
| `entity_system.ember` | fixed-array entity update |
| `text_formatter.ember` | f-strings, concatenation, `fmt*` |
| `map_inventory.ember` | full map lifecycle |
| `stack_queue.ember` | data structures over array handles |
| `retry_pattern.ember` | bounded try/catch retry |
| `thread_parallel_sum.ember` | parallel workers and joins |
| `coroutine_pipeline.ember` | lazy producer/consumer |
| `sync_queue.ember` | bounded non-blocking MPMC queue |
| `bootstrap_demo.ember` | command for the real two-generation bootstrap |

## Host-embedded examples

These intentionally depend on natives or lifecycle behavior supplied by their
host and should not be run directly with the stock CLI:

- `gain_plugin.ember` and `delay_plugin.ember` — run by `vst_dsp_harness`.
- `game_logic.ember` — run by `game_host`.
- `dynamic_registration.ember` — run with `--tick --tick-count N`; plain run
  returns its keep-loaded status, 1.

The production VST3 scripts and callback ABI examples are in
`examples/vst3_wrapper/`.
