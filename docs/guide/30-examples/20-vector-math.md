# Vector Math Walkthrough

> **STALENESS NOTICE (2026-07-11):** the file
> `examples/scripts/vector_math_demo.ember` **does not exist** in the tree.
> The "Full Source" below is illustrative. It uses `assert_eq_f32`, a
> **prism-host** native not registered by the standalone `ember` CLI. See
> `examples/scripts/` for the real shipped example scripts (`control.ember`,
> `struct.ember`, `string.ember`, `game_logic.ember`, `io_test.ember`,
> `dynamic_registration.ember`, `read_line_test.ember`, `fib.ember`).

This page walks through `examples/scripts/vector_math_demo.ember`, a small script that exercises `vec3`, `vec4`, `quat`, and `mat4` together the way a physics or graphics math layer would. It is not rendering- or game-specific, it is a tour of the math handle types and their operator overloads. For the full function reference behind every call here, see [20-api/40-math-vectors.md](../20-api/40-math-vectors.md).

## Full Source

```ember
fn vec3_length_squared(v: vec3) -> f32 {
    let dotted: vec3 = v * v;
    return dotted.vec3_x() + dotted.vec3_y() + dotted.vec3_z();
}

fn make_scale_matrix(sx: f32, sy: f32, sz: f32) -> mat4 {
    let m: mat4 = mat4_new();
    mat4_set(m, 0, 0, sx);
    mat4_set(m, 1, 1, sy);
    mat4_set(m, 2, 2, sz);
    mat4_set(m, 3, 3, 1.0f);
    return m;
}

fn main() -> i64 {
    let position: vec3 = vec3_new(0.0f, 10.0f, 0.0f);
    let velocity: vec3 = vec3_new(1.0f, -2.0f, 0.5f);
    let dt: f32 = 0.5f;
    let scaled_velocity: vec3 = vec3_new(velocity.vec3_x() * dt, velocity.vec3_y() * dt, velocity.vec3_z() * dt);
    let new_position: vec3 = position + scaled_velocity;
    assert_eq_f32(new_position.vec3_x(), 0.5f);
    assert_eq_f32(new_position.vec3_y(), 9.0f);
    assert_eq_f32(new_position.vec3_z(), 0.25f);

    let leg: vec3 = vec3_new(3.0f, 4.0f, 0.0f);
    assert_eq_f32(vec3_length_squared(leg), 25.0f);

    let base_color: vec4 = vec4_new(0.5f, 0.5f, 0.5f, 1.0f);
    let tint: vec4 = vec4_new(1.2f, 0.8f, 1.0f, 1.0f);
    let tinted: vec4 = base_color * tint;
    assert_eq_f32(tinted.vec4_x(), 0.6f);
    assert_eq_f32(tinted.vec4_y(), 0.4f);

    let turn_a: quat = quat_new(1.0f, 0.0f, 0.0f, 0.0f);
    let turn_b: quat = quat_new(0.0f, 1.0f, 0.0f, 0.0f);
    let combined: quat = turn_a * turn_b;
    assert_eq_f32(combined.quat_z(), 1.0f); // i*j = k

    let scale2: mat4 = make_scale_matrix(2.0f, 2.0f, 2.0f);
    let scale3: mat4 = make_scale_matrix(3.0f, 3.0f, 3.0f);
    let scale6: mat4 = scale2 * scale3;
    assert_eq_f32(mat4_get(scale6, 0, 0), 6.0f);
    return 0;
}
```

## vec3: Position and Velocity Integration

The first block in `main` is the smallest physics step there is: take a position, take a velocity, scale the velocity by a time delta, and add it to the position.

```ember
let position: vec3 = vec3_new(0.0f, 10.0f, 0.0f);
let velocity: vec3 = vec3_new(1.0f, -2.0f, 0.5f);
let dt: f32 = 0.5f;
let scaled_velocity: vec3 = vec3_new(velocity.vec3_x() * dt, velocity.vec3_y() * dt, velocity.vec3_z() * dt);
let new_position: vec3 = position + scaled_velocity;
```

`scaled_velocity` is built by hand, field by field, because there is no `vec3 * f32` overload in this script's call sequence: each component of `velocity` is read out with `.vec3_x()`, `.vec3_y()`, `.vec3_z()`, multiplied against the plain `f32` scalar `dt`, and the three results are packed back into a new `vec3` with `vec3_new`. Once `scaled_velocity` exists, `position + scaled_velocity` uses the registered `vec3 + vec3` overload, which adds component-wise, matching ordinary vector addition.

The three `assert_eq_f32` calls confirm the arithmetic: `0.0 + 1.0 * 0.5 = 0.5`, `10.0 + (-2.0) * 0.5 = 9.0`, and `0.0 + 0.5 * 0.5 = 0.25`. This is the shape of a single Euler integration step: `position = position + velocity * dt`, spelled out with explicit component access because `vec3` handles do not implicitly broadcast a scalar across their fields.

## vec3: Length-Squared via Self-Multiply

`vec3_length_squared` shows how to combine an operator overload with the component accessors to build a small piece of math the API does not hand you directly:

```ember
fn vec3_length_squared(v: vec3) -> f32 {
    let dotted: vec3 = v * v;
    return dotted.vec3_x() + dotted.vec3_y() + dotted.vec3_z();
}
```

`v * v` uses the `vec3 * vec3` overload, which multiplies component-wise: `(x*x, y*y, z*z)`. That is not the dot product by itself, it is the elementwise square of each field. Summing those three fields together (`dotted.vec3_x() + dotted.vec3_y() + dotted.vec3_z()`) is what actually produces the dot product of `v` with itself, which is exactly the squared length of the vector. Calling it on `vec3_new(3.0f, 4.0f, 0.0f)`, a 3-4-5 right triangle's legs, returns `9.0 + 16.0 + 0.0 = 25.0`, the square of the hypotenuse.

The pattern generalizes: component-wise multiply plus a field sum is how you build a dot product out of the `*` overload, since `*` on `vec3` never reduces to a scalar on its own.

## vec4: RGBA Tint

`vec4` follows the same component-wise rule as `vec3`, applied to four fields instead of three, which makes it a natural fit for color tinting:

```ember
let base_color: vec4 = vec4_new(0.5f, 0.5f, 0.5f, 1.0f);
let tint: vec4 = vec4_new(1.2f, 0.8f, 1.0f, 1.0f);
let tinted: vec4 = base_color * tint;
```

Treating `vec4` fields as RGBA channels, `base_color * tint` multiplies red against red, green against green, blue against blue, and alpha against alpha: `(0.5*1.2, 0.5*0.8, 0.5*1.0, 1.0*1.0)`, which is `(0.6, 0.4, 0.5, 1.0)`. The script only asserts the first two channels, `tinted.vec4_x()` is `0.6` and `tinted.vec4_y()` is `0.4`, but all four multiply the same way. This is the standard way to apply a tint or a mask to a color value: component-wise multiply, one channel at a time, with no cross-channel mixing.

## quat: the Hamilton Product Is Not Component-Wise

The `quat` block is the point in the script where component-wise thinking breaks, on purpose:

```ember
let turn_a: quat = quat_new(1.0f, 0.0f, 0.0f, 0.0f);
let turn_b: quat = quat_new(0.0f, 1.0f, 0.0f, 0.0f);
let combined: quat = turn_a * turn_b;
assert_eq_f32(combined.quat_z(), 1.0f); // i*j = k
```

`turn_a` is the quaternion basis element `i` (its `x` field is `1.0`, standing in for the imaginary unit `i`), and `turn_b` is the basis element `j` (its `y` field is `1.0`). If `*` on `quat` were component-wise like it is on `vec3` and `vec4`, `turn_a * turn_b` would come out to all zeros: `x` times `x` is `1*0`, `y` times `y` is `0*1`, every paired field has a zero on one side. Instead, `combined.quat_z()` is `1.0`.

That result only makes sense under the Hamilton product, the quaternion multiplication rule where `i * j = k`, not under elementwise multiplication. The `*` overload registered for `quat` performs the true Hamilton product: each output component is a weighted sum across all four input fields of both operands (following the standard `(w, x, y, z)` quaternion multiplication formula), not a simple pairing of matching fields. This is precisely why `combined.quat_z()` lands on `1.0`: multiplying the pure-`i` quaternion by the pure-`j` quaternion produces the pure-`k` quaternion, reproducing `i * j = k`, the defining identity of quaternion algebra.

The takeaway carries into any quaternion work you do in Ember: `quat * quat` composes two rotations, it is not the elementwise product you would get from `vec4 * vec4` even though a `quat` also carries four `f32` fields under the hood.

## mat4: Scale-Matrix Composition Is Not Component-Wise Either

The last block makes the same point again with matrices instead of quaternions, using `make_scale_matrix` as a small helper:

```ember
fn make_scale_matrix(sx: f32, sy: f32, sz: f32) -> mat4 {
    let m: mat4 = mat4_new();
    mat4_set(m, 0, 0, sx);
    mat4_set(m, 1, 1, sy);
    mat4_set(m, 2, 2, sz);
    mat4_set(m, 3, 3, 1.0f);
    return m;
}
```

`mat4_new` returns a matrix handle (its initial contents come from the host, see [20-api/40-math-vectors.md](../20-api/40-math-vectors.md) for the exact default), and `mat4_set(m, row, col, value)` writes one element at a time. `make_scale_matrix` builds a standard 4x4 scale matrix: `sx`, `sy`, `sz` on the diagonal, `1.0` in the bottom-right homogeneous corner, and (implicitly, from `mat4_new`'s default) zeros everywhere else off-diagonal.

```ember
let scale2: mat4 = make_scale_matrix(2.0f, 2.0f, 2.0f);
let scale3: mat4 = make_scale_matrix(3.0f, 3.0f, 3.0f);
let scale6: mat4 = scale2 * scale3;
assert_eq_f32(mat4_get(scale6, 0, 0), 6.0f);
```

If `mat4 * mat4` multiplied matching cells together the way `vec3 * vec3` multiplies matching fields, then cell `(0, 0)` of `scale6` would be `2.0 * 2.0 = 4.0` (both matrices only have a nonzero value at `(0,0)` from their own diagonal). Instead, `mat4_get(scale6, 0, 0)` comes out to `6.0`. That is `2.0 * 3.0`, the product of the two scale factors, which is exactly what you get from proper matrix multiplication: row `0` of `scale2` dotted with column `0` of `scale3`. Since both are diagonal matrices, that row-times-column dot product collapses to a single term, the product of the two diagonal entries, but it is still a dot product under the hood, not a cell-by-cell multiply.

The `*` overload registered for `mat4` is standard row-by-column matrix multiplication, the same operation that lets you compose two scale matrices into one combined scale, or in general compose any two affine or linear transforms into a single `mat4`. Composing a uniform 2x scale with a uniform 3x scale produces a uniform 6x scale, which is what `scale6`'s diagonal entries confirm.

## The Common Thread

Three of these four handle types overload `*` as a plain component-wise multiply: `vec3`, `vec4`, and (implicitly, through `vec3_length_squared`'s use of `v * v`) any other vector type in the math API follows that rule. `quat` and `mat4` do not: their `*` overloads implement the mathematically meaningful composition operator for their domain (the Hamilton product for quaternions, row-by-column multiplication for matrices), because that is the operation that actually matters when you compose rotations or transforms. Reaching for `*` on any math handle type without checking which family it belongs to is the easiest way to get a silently wrong result rather than a compile error, since sema resolves the overload statically and happily accepts either kind of multiplication as long as the operand types match. See [20-api/40-math-vectors.md](../20-api/40-math-vectors.md) for the full function and operator reference across `vec2`, `vec3`, `vec4`, `quat`, and `mat4`.
