# Math and Vectors

## Scalar math

### `f32`

```text
sqrt sin cos tan atan exp log floor ceil abs round : (f32) -> f32
atan2 : (f32, f32) -> f32
```

### `f64`

```text
sqrt_f64 sin_f64 cos_f64 tan_f64 atan_f64 exp_f64 log_f64 log2_f64
log10_f64 floor_f64 ceil_f64 abs_f64 round_f64 trunc_f64 : (f64) -> f64
atan2_f64 pow_f64 fmod_f64 min_f64 max_f64 : (f64, f64) -> f64
clamp_f64 : (f64, f64, f64) -> f64
```

### `i64`

```text
abs_i64 : (i64) -> i64
min_i64 max_i64 : (i64, i64) -> i64
clamp_i64 : (i64, i64, i64) -> i64
```

Angles are radians. Domain/NaN/infinity behavior follows the C++ standard-library math functions used by the extension.

```ember
fn scalar_math() -> i64 {
    let hyp: f64 = sqrt_f64(pow_f64(3.0, 2.0) + pow_f64(4.0, 2.0));
    let bounded: i64 = clamp_i64(100, 0, 42);
    return (hyp as i64) + bounded;
}
```

The standalone extension does not register prism's `aim_atan2` or `clamp`; use `atan2`/`atan2_f64` and the typed clamp functions above.

## Handle model

`vec2`, `vec3`, `vec4`, `quat`, and `mat4` are typed host-owned handles. Accessors can use free-function or method syntax.

### `vec2`

```text
vec2_new(x: f32, y: f32) -> vec2
vec2_x(v: vec2) -> f32        vec2_y(v: vec2) -> f32
vec2_set_x(v: vec2, x: f32)   vec2_set_y(v: vec2, y: f32)
```

Operators: component-wise `+`, `-`, `*`, and content `==`.

### `vec3`

```text
vec3_new(x: f32, y: f32, z: f32) -> vec3
vec3_x/y/z(v: vec3) -> f32
vec3_set_x/y/z(v: vec3, value: f32) -> void
```

Operators: component-wise `+`, `-`, `*`, and content `==`. There is no scalar-multiply, dot, or cross overload.

```ember
fn length_squared(v: vec3) -> f32 {
    let squared: vec3 = v * v;
    return squared.vec3_x() + squared.vec3_y() + squared.vec3_z();
}
```

### `vec4`

```text
vec4_new(x: f32, y: f32, z: f32, w: f32) -> vec4
vec4_x/y/z/w(v: vec4) -> f32
vec4_set_x/y/z/w(v: vec4, value: f32) -> void
```

Operators: component-wise `+`, `-`, `*`, and content `==`.

### `quat`

```text
quat_new(x: f32, y: f32, z: f32, w: f32) -> quat
quat_x/y/z/w(q: quat) -> f32
quat_set_x/y/z/w(q: quat, value: f32) -> void
```

Operators: component-wise `+` and `-`, Hamilton-product `*`, and content `==`. Multiplication is noncommutative.

```ember
fn quaternion_check() -> i64 {
    let i: quat = quat_new(1.0f, 0.0f, 0.0f, 0.0f);
    let j: quat = quat_new(0.0f, 1.0f, 0.0f, 0.0f);
    let k: quat = i * j;
    return k.quat_z() == 1.0f ? 1 : 0;
}
```

### `mat4`

```text
mat4_new() -> mat4                 # all zeros
mat4_identity() -> mat4
mat4_get(m: mat4, row: i64, col: i64) -> f32
mat4_set(m: mat4, row: i64, col: i64, value: f32) -> void
```

Operators: true row-major matrix `*` and content `==`. There are no matrix `+` or `-` overloads.

```ember
fn matrix_check() -> i64 {
    let a: mat4 = mat4_identity();
    let b: mat4 = mat4_identity();
    let c: mat4 = a * b;
    return mat4_get(c, 0, 0) == 1.0f ? 1 : 0;
}
```

Rows and columns are zero-based. Invalid indices return `0.0` or ignore the write in the host accessor; unlike language array indexing, these accessors do not raise `TrapReason::BoundsCheck`.

## Multiplication summary

| Type | `*` means |
|---|---|
| `vec2`, `vec3`, `vec4` | component-wise multiplication |
| `quat` | Hamilton product |
| `mat4` | row-by-column 4x4 matrix product |

See the [Vector Math walkthrough](../30-examples/20-vector-math.md) for a complete runnable example.
