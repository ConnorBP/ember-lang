# Math and Vectors

This page documents every native scalar math function and every native vector/quaternion/matrix type: `vec2`, `vec3`, `vec4`, `quat`, and `mat4`. See [API Overview](00-overview.md) for how signatures are written and how method-call sugar works.

All five vector-family types (`vec2`, `vec3`, `vec4`, `quat`, `mat4`) are opaque `i64` handles from Ember's point of view. You never see their fields directly. You create one with a `_new` (or `_identity`) call, read and write components through accessor functions, and combine them with operator overloads. Every accessor is also callable with method syntax, so `v.vec3_x()` and `vec3_x(v)` are the same call.

> **WARNING:** The `*` operator does not mean the same thing across this page. For `vec2`, `vec3`, and `vec4` it is component-wise multiplication. For `quat` and `mat4` it is the real mathematical product (Hamilton product and matrix product). Same symbol, different math. See the callout below before you use `*` on any of these types.

## The Times Operator Distinction

This is the single most important thing to know about this API surface.

- `vec2 * vec2`, `vec3 * vec3`, `vec4 * vec4` all multiply component by component. `a * b` gives you `(a.x * b.x, a.y * b.y, ...)`. There is no dot product or cross product overload for `vec3`; if you need those, compute them by hand from the components.
- `quat * quat` is the true Hamilton quaternion product, not component-wise. Quaternion multiplication is noncommutative: `p * q` is generally not equal to `q * p`.
- `mat4 * mat4` is the true row-major 4x4 matrix product, not component-wise. Matrix multiplication is also noncommutative.

If you mentally read every `*` as "multiply the matching fields," you will get the right answer for `vec2`/`vec3`/`vec4` and the wrong answer for `quat`/`mat4`.

### Example: Hamilton product on quat (i times j equals k)

```ember
// composing two "quarter turns" (i and j) via the Hamilton product
let turn_a: quat = quat_new(1.0f, 0.0f, 0.0f, 0.0f); // this is "i"
let turn_b: quat = quat_new(0.0f, 1.0f, 0.0f, 0.0f); // this is "j"
let combined: quat = turn_a * turn_b;
assert_eq_f32(combined.quat_z(), 1.0f); // i * j = k
```

`turn_a` and `turn_b` each have only one nonzero component, so this looks like it should behave like component-wise multiply (which would give an all-zero result, since the nonzero components do not line up). It does not. `quat * quat` runs the Hamilton product formula, and the classic identity `i * j = k` falls out: the result has `z` equal to `1.0` and `x`, `y`, `w` equal to `0.0`.

### Example: matrix product composing two scales

```ember
fn make_scale_matrix(sx: f32, sy: f32, sz: f32) -> mat4 {
    let m: mat4 = mat4_new();
    mat4_set(m, 0, 0, sx);
    mat4_set(m, 1, 1, sy);
    mat4_set(m, 2, 2, sz);
    mat4_set(m, 3, 3, 1.0f);
    return m;
}

// a uniform scale, applied twice, doubles then triples
let scale2: mat4 = make_scale_matrix(2.0f, 2.0f, 2.0f);
let scale3: mat4 = make_scale_matrix(3.0f, 3.0f, 3.0f);
let scale6: mat4 = scale2 * scale3;
assert_eq_f32(mat4_get(scale6, 0, 0), 6.0f);
assert_eq_f32(mat4_get(scale6, 1, 1), 6.0f);
assert_eq_f32(mat4_get(scale6, 2, 2), 6.0f);
```

`scale2` scales by 2 on each axis and `scale3` scales by 3 on each axis. `scale2 * scale3` is a real 4x4 matrix product, so the diagonal entries multiply through (`2 * 3 = 6`) exactly as composing two scale transforms should. If `mat4 * mat4` were component-wise instead, the off-diagonal zero entries would stay zero either way here, which is exactly why matrix bugs like this are easy to miss until you compose a rotation with something else. Always reach for the true matrix product mentally when you see `mat4 * mat4`.

## Scalar Math

Free functions operating on `f32` values. None of these are methods on a handle type; call them directly.

| Function | Returns | Description |
|---|---|---|
| `sqrt(v: f32)` | `f32` | Square root of `v` |
| `sin(v: f32)` | `f32` | Sine of `v`, in radians |
| `cos(v: f32)` | `f32` | Cosine of `v`, in radians |
| `tan(v: f32)` | `f32` | Tangent of `v`, in radians |
| `aim_atan2(y: f32, x: f32)` | `f32` | `atan2(y, x)`, the angle of the vector `(x, y)` from the positive x-axis |
| `clamp(v: f32, lo: f32, hi: f32)` | `f32` | Clamps `v` into the range `lo` to `hi` |

```ember
let angle: f32 = aim_atan2(3.0f, 4.0f);
let capped: f32 = clamp(sin(angle), -0.5f, 0.5f);
print_f32(sqrt(2.0f));
```

## vec2

Two `f32` components, `x` and `y`. An opaque `i64` handle.

| Function | Returns | Description |
|---|---|---|
| `vec2_new(x: f32, y: f32)` | `i64` | Creates a new `vec2` handle |
| `vec2_x(v: i64)` | `f32` | Reads the `x` component |
| `vec2_y(v: i64)` | `f32` | Reads the `y` component |
| `vec2_set_x(v: i64, x: f32)` | (none) | Writes the `x` component |
| `vec2_set_y(v: i64, y: f32)` | (none) | Writes the `y` component |

Operator overloads:

| Expression | Returns | Description |
|---|---|---|
| `vec2 + vec2` | `vec2` | Component-wise add |
| `vec2 - vec2` | `vec2` | Component-wise subtract |
| `vec2 * vec2` | `vec2` | Component-wise multiply |
| `vec2 == vec2` | `bool` | Component-wise equality |

```ember
let a: vec2 = vec2_new(1.0f, 2.0f);
let b: vec2 = vec2_new(3.0f, 4.0f);
let sum: vec2 = a + b;
let scaled: vec2 = a * b; // (1*3, 2*4) = (3, 8), NOT a dot product
assert_eq_f32(sum.vec2_x(), 4.0f);
assert_eq_f32(scaled.vec2_y(), 8.0f);
```

## vec3

Three `f32` components, `x`, `y`, `z`. An opaque `i64` handle. Commonly used for positions, velocities, and other 3D quantities.

| Function | Returns | Description |
|---|---|---|
| `vec3_new(x: f32, y: f32, z: f32)` | `i64` | Creates a new `vec3` handle |
| `vec3_x(v: i64)` | `f32` | Reads the `x` component |
| `vec3_y(v: i64)` | `f32` | Reads the `y` component |
| `vec3_z(v: i64)` | `f32` | Reads the `z` component |
| `vec3_set_x(v: i64, val: f32)` | (none) | Writes the `x` component |
| `vec3_set_y(v: i64, val: f32)` | (none) | Writes the `y` component |
| `vec3_set_z(v: i64, val: f32)` | (none) | Writes the `z` component |

Operator overloads:

| Expression | Returns | Description |
|---|---|---|
| `vec3 + vec3` | `vec3` | Component-wise add |
| `vec3 - vec3` | `vec3` | Component-wise subtract |
| `vec3 * vec3` | `vec3` | Component-wise multiply, NOT dot or cross product |
| `vec3 == vec3` | `bool` | Component-wise equality |

`vec3` has no dot-product or cross-product overload. `vec3 * vec3` is strictly component-wise, so a length-squared helper has to sum the products of matching components by hand:

```ember
fn vec3_length_squared(v: vec3) -> f32 {
    let dotted: vec3 = v * v;
    return dotted.vec3_x() + dotted.vec3_y() + dotted.vec3_z();
}

// a 3-4-5 right triangle's hypotenuse, squared, is 25
let leg: vec3 = vec3_new(3.0f, 4.0f, 0.0f);
assert_eq_f32(vec3_length_squared(leg), 25.0f);
```

Accessors work equally well as free functions or as methods:

```ember
let position: vec3 = vec3_new(0.0f, 10.0f, 0.0f);
let velocity: vec3 = vec3_new(1.0f, -2.0f, 0.5f);
let dt: f32 = 0.5f;
let scaled_velocity: vec3 = vec3_new(velocity.vec3_x() * dt, velocity.vec3_y() * dt, velocity.vec3_z() * dt);
let new_position: vec3 = position + scaled_velocity;
assert_eq_f32(new_position.vec3_y(), 9.0f);
```

## vec4

Four `f32` components, `x`, `y`, `z`, `w`. An opaque `i64` handle. Commonly used for RGBA colors, homogeneous coordinates, or any four-wide quantity.

| Function | Returns | Description |
|---|---|---|
| `vec4_new(x: f32, y: f32, z: f32, w: f32)` | `i64` | Creates a new `vec4` handle |
| `vec4_x(v: i64)` | `f32` | Reads the `x` component |
| `vec4_y(v: i64)` | `f32` | Reads the `y` component |
| `vec4_z(v: i64)` | `f32` | Reads the `z` component |
| `vec4_w(v: i64)` | `f32` | Reads the `w` component |
| `vec4_set_x(v: i64, val: f32)` | (none) | Writes the `x` component |
| `vec4_set_y(v: i64, val: f32)` | (none) | Writes the `y` component |
| `vec4_set_z(v: i64, val: f32)` | (none) | Writes the `z` component |
| `vec4_set_w(v: i64, val: f32)` | (none) | Writes the `w` component |

Operator overloads:

| Expression | Returns | Description |
|---|---|---|
| `vec4 + vec4` | `vec4` | Component-wise add |
| `vec4 - vec4` | `vec4` | Component-wise subtract |
| `vec4 * vec4` | `vec4` | Component-wise multiply |
| `vec4 == vec4` | `bool` | Component-wise equality |

A `vec4 * vec4` component-wise multiply is exactly what you want for tinting an RGBA color, since each channel scales independently:

```ember
let base_color: vec4 = vec4_new(0.5f, 0.5f, 0.5f, 1.0f);
let tint: vec4 = vec4_new(1.2f, 0.8f, 1.0f, 1.0f);
let tinted: vec4 = base_color * tint;
assert_eq_f32(tinted.vec4_x(), 0.6f);
assert_eq_f32(tinted.vec4_y(), 0.4f);
```

## quat

Four `f32` components, `x`, `y`, `z`, `w`, representing a quaternion. An opaque `i64` handle.

| Function | Returns | Description |
|---|---|---|
| `quat_new(x: f32, y: f32, z: f32, w: f32)` | `i64` | Creates a new `quat` handle |
| `quat_x(v: i64)` | `f32` | Reads the `x` component |
| `quat_y(v: i64)` | `f32` | Reads the `y` component |
| `quat_z(v: i64)` | `f32` | Reads the `z` component |
| `quat_w(v: i64)` | `f32` | Reads the `w` component |
| `quat_set_x(v: i64, val: f32)` | (none) | Writes the `x` component |
| `quat_set_y(v: i64, val: f32)` | (none) | Writes the `y` component |
| `quat_set_z(v: i64, val: f32)` | (none) | Writes the `z` component |
| `quat_set_w(v: i64, val: f32)` | (none) | Writes the `w` component |

Operator overloads:

| Expression | Returns | Description |
|---|---|---|
| `quat + quat` | `quat` | Component-wise add |
| `quat - quat` | `quat` | Component-wise subtract |
| `quat * quat` | `quat` | TRUE Hamilton quaternion product, NOT component-wise (see below) |
| `quat == quat` | `bool` | Component-wise equality |

`+` and `-` on `quat` are plain component-wise add and subtract, same as the vector types. `*` is different: it is the real Hamilton product, used to compose two rotations. See "The Times Operator Distinction" near the top of this page for the full `i * j = k` example. Because the Hamilton product is noncommutative, `p * q` and `q * p` generally give different results, so order matters when composing rotations.

## mat4

A row-major 4x4 array of 16 `f32` values. An opaque `i64` handle. Rows and columns are addressed with zero-based indices `0` through `3`.

| Function | Returns | Description |
|---|---|---|
| `mat4_new()` | `i64` | Creates a new `mat4` handle, all 16 entries zero |
| `mat4_identity()` | `i64` | Creates a new `mat4` handle set to the identity matrix |
| `mat4_get(m: i64, row: i64, col: i64)` | `f32` | Reads the entry at `row`, `col` |
| `mat4_set(m: i64, row: i64, col: i64, val: f32)` | (none) | Writes the entry at `row`, `col` |

Operator overloads:

| Expression | Returns | Description |
|---|---|---|
| `mat4 * mat4` | `mat4` | TRUE row-major 4x4 matrix product, NOT component-wise |
| `mat4 == mat4` | `bool` | Component-wise equality |

`mat4` has no `+`, `-`, `!=`, `<`, `>`, `<=`, or `>=` overloads: only `*` (matrix product) and `==` (component-wise equality) exist. Unlike `vec2`/`vec3`/`vec4`, there is no addition overload for `mat4`; combine matrices by building them with `mat4_set` and composing with `*`.

`mat4_new()` and `mat4_identity()` are both valid starting points, but they are not the same. `mat4_new()` gives you all zeros, which is rarely useful on its own except as scratch space you intend to fill entirely with `mat4_set`. `mat4_identity()` gives you the identity matrix, the correct starting point for building up a transform by multiplying in scales, rotations, and translations.

See "The Times Operator Distinction" near the top of this page for the full scale-composition example (`make_scale_matrix`, `scale2 * scale3`). Because matrix multiplication is noncommutative, the order you multiply transforms in changes the result: `a * b` applies differently than `b * a` unless the two matrices happen to commute.

```ember
let m: mat4 = mat4_identity();
assert_eq_f32(mat4_get(m, 0, 0), 1.0f);
assert_eq_f32(mat4_get(m, 0, 1), 0.0f);

mat4_set(m, 3, 0, 5.0f); // stash a translation-style value into row 3
assert_eq_f32(mat4_get(m, 3, 0), 5.0f);
```

---

> **NOTE:** For a complete script exercising every type on this page together (vec3 physics integration, vec4 color tinting, the quat Hamilton product, and mat4 scale composition), see the vector math example script in `examples/scripts/vector_math_demo.ember`.
