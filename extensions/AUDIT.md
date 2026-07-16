# Ember extension audit

## Status and scope

This file preserves the **historical Prism relocation audit** that created the
first six Ember extensions. It is not the current extension inventory. The
current tree contains 17 extension libraries (15 native/addon + 2 pass); see
[`README.md`](README.md) for the authoritative list.

The original audit classified registrations from these Prism host builders:

- `prism/src/prism/prism_script_host.cpp`
- `prism/src/prism/shader_api.cpp`
- `prism/src/prism/prism_panel_api.cpp`

A registration was relocated only when it was generic, host-independent, and
already part of the planned standard addon set. Product/process/render/UI
operations remained in the embedding host.

## Original relocation result

| Family | Original disposition | Current Ember home |
|---|---|---|
| vec2/vec3/vec4 natives and overloads | relocated | `extensions/vec/` |
| quat natives and overloads | relocated | `extensions/quat/` |
| mat4 natives and overloads | relocated | `extensions/mat/` |
| owned string natives and overloads | relocated | `extensions/string/` |
| array natives | relocated | `extensions/array/` |
| sqrt/sin/cos/tan math natives | relocated | `extensions/math/` (subsequently expanded) |
| process memory access | retained by host | not an Ember extension |
| view/render/shader resources and drawing | retained by host | not an Ember extension |
| GUI/panel/widget APIs | retained by host | not an Ember extension |
| host-specific print/assert/timer/test helpers | retained by host | not part of the relocation |
| host-specific input, network, bitmap/font/sound operations | retained by host | not part of the relocation |

The original overload migration was:

| Type | Operators |
|---|---|
| `vec2`, `vec3`, `vec4` | add, subtract, multiply, equality |
| `quat` | add, subtract, Hamilton multiply, equality |
| `mat4` | matrix multiply, equality |
| `string` | concatenate, equality |

## What changed after the audit

The project's generic extension surface grew substantially after the
relocation. These additions were authored directly in Ember and therefore were
never Prism-audit candidates:

- native/addon: `map`, `sync`, `thread`, `coroutine`, `lifecycle`, `io`,
  `call_raw`, `gc`, `audio`;
- pass extensions: `opt`, `obf`.

Several original extensions also grew beyond the names in the old table:

- `math` now includes broad f32/f64 elementary math and min/max/clamp helpers;
- `string` includes find, substring, and `fmt1`-`fmt4`;
- `array` includes typed pop/clear/remove operations;
- string-literal decryption is inline compiler codegen; no `__str_decrypt`
  host native is required;
- `call_raw` now includes the EMBM v1/v2 module-loader/query/free surface in
  addition to raw page creation/call/free.

## Current boundary rule

An Ember extension must remain reusable by unrelated hosts. Generic language
runtime facilities belong here. APIs coupled to one host's process model,
renderer, UI, resource system, or domain objects remain in that host.

Native extensions use `BindingBuilder`/`NativeSig`, optional overload tables,
and explicit host registration. Pass extensions use `EmberPassRegistry` and
`register_passes`. Neither category changes the parser grammar merely by being
linked.

For the exact current capabilities, permissions, platform limitations, target
names, and all 25 pass names, use [`extensions/README.md`](README.md) and the
extension headers as the source of truth.
