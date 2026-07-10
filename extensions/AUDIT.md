# ember extensions - prism native-registration audit

The Section 6 audit (`../docs/planning/RESTRUCTURE_PLAN.md`): classify every
`ember::NativeSig` registration and `ember::OpOverloadTable` entry in
prism as **cheat-specific** (stays in prism) or **general-purpose**
(relocates to `ember/extensions/`). Source of the registrations:
`prism/src/prism/prism_script_host.cpp` (`BuildScriptHostNatives` +
`RegisterScriptHostOverloads`), `prism/src/prism/shader_api.cpp`
(`build_shader_natives`), `prism/src/prism/prism_panel_api.cpp`
(`build_panel_natives`).

Classification rule: a registration is **general-purpose** iff it is
(a) unambiguously non-cheat - not tied to reading/writing a game
process or rendering an overlay/GUI, and (b) in the `../docs/ROADMAP.md`
Tier 0 standard addon set (`array<T>`, `map<K,V>`, `string`, `math`
(sin/cos/sqrt/floor/ceil/abs/min/max/pow), `vec2/3/4`/`quat`/`mat4`).
Anything else stays in prism. No new addons are authored (YAGNI).

## NativeSig registrations (BuildScriptHostNatives)

| Native name | Family | Classification | Disposition |
|---|---|---|---|
| `print_i64` | print/IO (host sink) | cheat-host-coupled | stays (prism) |
| `print_f32` | print/IO (host sink) | cheat-host-coupled | stays (prism) |
| `print_str` | print/IO (host sink) | cheat-host-coupled | stays (prism) |
| `assert_eq_i64` | assert (host sink + counter) | cheat-host-coupled | stays (prism) |
| `assert_eq_f32` | assert (host sink + counter) | cheat-host-coupled | stays (prism) |
| `assert_eq_bool` | assert (host sink + counter) | cheat-host-coupled | stays (prism) |
| `assert_eq_str` | assert (host sink + counter) | cheat-host-coupled | stays (prism) |
| `ru64` | proc.* (process read) | cheat-specific | stays (prism) |
| `ru32` | proc.* (process read) | cheat-specific | stays (prism) |
| `r32` | proc.* (process read) | cheat-specific | stays (prism) |
| `rf32` | proc.* (process read) | cheat-specific | stays (prism) |
| `wu64` | proc.* (process write) | cheat-specific | stays (prism) |
| `wf32` | proc.* (process write) | cheat-specific | stays (prism) |
| `r8` | proc.* (process read) | cheat-specific | stays (prism) |
| `get_tickcount64` | host-process timer (cheat timing) | cheat-host-coupled | stays (prism) |
| `clamp` | math helper (not in ROADMAP list) | borderline | stays (prism) |
| `get_view_width` | view/render | cheat-specific | stays (prism) |
| `get_view_height` | view/render | cheat-specific | stays (prism) |
| `get_view_scale` | view/render | cheat-specific | stays (prism) |
| `get_view` | view/render | cheat-specific | stays (prism) |
| `min_max` | math helper (slice ABI, not in ROADMAP) | borderline | stays (prism) |
| `double_it` | test toy | not a real addon | stays (prism) |
| `add_val` | test toy | not a real addon | stays (prism) |
| `fp_to_ieee` | numeric bit-cast (GPU shader use) | not in ROADMAP | stays (prism) |
| `ieee_to_fp` | numeric bit-cast (GPU shader use) | not in ROADMAP | stays (prism) |
| `ref_process_native` | proc.* (process attach) | cheat-specific | stays (prism) |
| `get_client_entity_native` | game entity | cheat-specific | stays (prism) |
| `world_to_screen_native` | render (w2s) | cheat-specific | stays (prism) |
| `log_u64` | print/IO (host sink) | cheat-host-coupled | stays (prism) |
| `aim_atan2` | cheat-named (aimbot) | cheat-specific | stays (prism) |
| `create_subtab`..`widget_add_child_single` (×21) | GUI/panel UI | cheat-specific | stays (prism) |
| `sqrt` | math (ROADMAP Tier 0) | **general-purpose** | → `ember_ext_math` |
| `sin` | math (ROADMAP Tier 0) | **general-purpose** | → `ember_ext_math` |
| `cos` | math (ROADMAP Tier 0) | **general-purpose** | → `ember_ext_math` |
| `tan` | math (ROADMAP Tier 0) | **general-purpose** | → `ember_ext_math` |
| `str_compare` | raw-ptr string op (not mutable string) | not in ROADMAP | stays (prism) |
| `str_length` | raw-ptr string op (not mutable string) | not in ROADMAP | stays (prism) |
| `read_bulk` | proc.* (process read into array) | cheat-specific | stays (prism) |
| `inject_mouse_delta` | input injection | cheat-specific | stays (prism) |
| `is_sound_playing` | audio | cheat-specific | stays (prism) |
| `play_sound_native` | audio | cheat-specific | stays (prism) |
| `load_sound_native` | audio | cheat-specific | stays (prism) |
| `get_interface_native` | game interface | cheat-specific | stays (prism) |
| `http_get_native` | network | cheat-specific | stays (prism) |
| `write_temp_file_native` | fs | cheat-specific | stays (prism) |
| `delete_file_native` | fs | cheat-specific | stays (prism) |
| `create_bitmap_from_bytes_native` | render resource | cheat-specific | stays (prism) |
| `create_font_from_bytes_native` | render resource | cheat-specific | stays (prism) |
| `load_sound_http` | audio/network | cheat-specific | stays (prism) |
| `array_new` | array<T> (ROADMAP Tier 0) | **general-purpose** | → `ember_ext_array` |
| `array_length` | array<T> | **general-purpose** | → `ember_ext_array` |
| `array_resize` | array<T> | **general-purpose** | → `ember_ext_array` |
| `array_set_u8` | array<T> | **general-purpose** | → `ember_ext_array` |
| `array_get_u8` | array<T> | **general-purpose** | → `ember_ext_array` |
| `array_set_f32` | array<T> | **general-purpose** | → `ember_ext_array` |
| `array_get_f32` | array<T> | **general-purpose** | → `ember_ext_array` |
| `array_set_i64` | array<T> | **general-purpose** | → `ember_ext_array` |
| `array_get_i64` | array<T> | **general-purpose** | → `ember_ext_array` |
| `array_push_u8` | array<T> | **general-purpose** | → `ember_ext_array` |
| `vec3_new`/`vec3_x`/`vec3_y`/`vec3_z`/`vec3_set_x`/`vec3_set_y`/`vec3_set_z` (×7) | vec3 (ROADMAP Tier 0) | **general-purpose** | → `ember_ext_vec` |
| `vec2_new`/`vec2_x`/`vec2_y`/`vec2_set_x`/`vec2_set_y` (×5) | vec2 (ROADMAP Tier 0) | **general-purpose** | → `ember_ext_vec` |
| `vec4_new`/`vec4_x`/`vec4_y`/`vec4_z`/`vec4_w`/`vec4_set_x`/`vec4_set_y`/`vec4_set_z`/`vec4_set_w` (×9) | vec4 (ROADMAP Tier 0) | **general-purpose** | → `ember_ext_vec` |
| `quat_new`/`quat_x`/`quat_y`/`quat_z`/`quat_w`/`quat_set_x`/`quat_set_y`/`quat_set_z`/`quat_set_w` (×9) | quat (ROADMAP Tier 0) | **general-purpose** | → `ember_ext_quat` |
| `mat4_new`/`mat4_identity`/`mat4_get`/`mat4_set` (×4) | mat4 (ROADMAP Tier 0) | **general-purpose** | → `ember_ext_mat` |
| `string_new` | string (ROADMAP Tier 0) | **general-purpose** | → `ember_ext_string` |
| `string_from_slice` | string | **general-purpose** | → `ember_ext_string` |
| `string_length` | string | **general-purpose** | → `ember_ext_string` |
| `string_char_at` | string | **general-purpose** | → `ember_ext_string` |
| `string_from_i64` | string | **general-purpose** | → `ember_ext_string` |
| `string_from_f32` | string | **general-purpose** | → `ember_ext_string` |
| `string_from_f64` | string | **general-purpose** | → `ember_ext_string` |
| `string_from_bool` | string | **general-purpose** | → `ember_ext_string` |
| `string_identity` | string | **general-purpose** | → `ember_ext_string` |
| `print_string` | print/IO (host sink) | cheat-host-coupled | stays (prism) |

String encryption is now pure codegen (no host native): an encrypted literal
is decrypted inline into a compiler-hidden temp frame slot at each use site
(see codegen's StringLit eval case / alloc_str_temp). The old
`__str_decrypt` host-contract native was removed; there is nothing for a host
to register.

## OpOverloadTable entries (RegisterScriptHostOverloads)

| Type | Op | Backing fn | Classification | Disposition |
|---|---|---|---|---|
| `vec3` | Add/Sub/Mul/Eq | `n_vec3_add`/`_sub`/`_mul`/`_eq` | **general-purpose** | → `ember_ext_vec` |
| `vec2` | Add/Sub/Mul/Eq | `n_vec2_*` | **general-purpose** | → `ember_ext_vec` |
| `vec4` | Add/Sub/Mul/Eq | `n_vec4_*` | **general-purpose** | → `ember_ext_vec` |
| `quat` | Add/Sub/Mul(Hamilton)/Eq | `n_quat_*` | **general-purpose** | → `ember_ext_quat` |
| `mat4` | Mul(4×4)/Eq | `n_mat4_*` | **general-purpose** | → `ember_ext_mat` |
| `string` | Add(concat)/Eq | `n_string_concat`/`n_string_eq` | **general-purpose** | → `ember_ext_string` |

## build_shader_natives (shader_api.cpp) - all cheat-specific (render), stay in prism

`create_shader`, `create_vertex_buffer`, `create_index_buffer`,
`create_constant_buffer`, `create_blend_state`, `create_sampler`,
`create_texture`, `create_render_target`, `create_depth_buffer`,
`create_structured_buffer`, `create_compute_shader`,
`create_rasterizer_state`, `create_depth_stencil_state`, `load_mesh*`,
`load_texture*`, `create_bitmap`, `create_mesh_raw`, `get_mesh_stride`,
`get_font*`, `create_font*`, `destroy_*`, `draw_rect_filled`,
`draw_text`, `update_structured_buffer`,
`custom_draw`/`custom_draw_indexed`/`custom_bind_constant_buffer`/
`custom_update_texture` (all reach into `GetArrayBytes` for array<u8>
handles, but the natives themselves are render-surface). All stay in
prism.

## build_panel_natives (prism_panel_api.cpp) - all cheat-specific (GUI), stay in prism

`register_gui_panel`, `unregister_gui_panel`, `begin_panel`,
`end_panel`, `gui_text`, `gui_label`, `gui_slider_float`,
`gui_slider_int`, `gui_checkbox`, `gui_button`, `gui_separator`,
`gui_same_line`. All stay in prism.

## prism's cheat overlay host TU - cheat-specific, stays in prism

The cheat overlay host's TU in `prism/src/prism/` builds its own
`ember::OpOverloadTable overloads;` for the overlay. That is a
cheat-specific host and stays entirely in prism (not relocated, not
brought into `ember/`).

## Summary

- **Relocated to `ember/extensions/`**: vec2/vec3/vec4, quat, mat4,
  string, array<T>, math (sqrt/sin/cos/tan). Six extensions.
- **Stayed in prism**: everything else (proc.*, render/view/shader,
  gui/panel, host-sink print/assert, host-process timer, cheat-named
  audio/font/bitmap/http/fs/input, numeric helpers not in the ROADMAP
  list, test toys, the cheat overlay host's own overload table). String
  encryption needs no host backing (pure codegen).
- **No new addons authored** (YAGNI). Only existing, provably-non-cheat,
  ROADMAP-Tier-0 extensions moved.
