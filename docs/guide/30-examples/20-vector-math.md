# Vector Math Walkthrough

Save this complete standalone example as `vector_math.ember`.

```ember
fn nearly_equal(a: f32, b: f32) -> bool {
    return abs(a - b) < 0.0001f;
}

fn length_squared(v: vec3) -> f32 {
    let squared: vec3 = v * v;
    return squared.vec3_x() + squared.vec3_y() + squared.vec3_z();
}

fn scale_matrix(sx: f32, sy: f32, sz: f32) -> mat4 {
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
    let step: vec3 = vec3_new(
        velocity.vec3_x() * dt,
        velocity.vec3_y() * dt,
        velocity.vec3_z() * dt
    );
    let next: vec3 = position + step;
    if (!nearly_equal(next.vec3_x(), 0.5f)) { return 1; }
    if (!nearly_equal(next.vec3_y(), 9.0f)) { return 2; }
    if (!nearly_equal(next.vec3_z(), 0.25f)) { return 3; }

    let leg: vec3 = vec3_new(3.0f, 4.0f, 0.0f);
    if (!nearly_equal(length_squared(leg), 25.0f)) { return 4; }

    let base: vec4 = vec4_new(0.5f, 0.5f, 0.5f, 1.0f);
    let tint: vec4 = vec4_new(1.2f, 0.8f, 1.0f, 1.0f);
    let color: vec4 = base * tint;
    if (!nearly_equal(color.vec4_x(), 0.6f)) { return 5; }
    if (!nearly_equal(color.vec4_y(), 0.4f)) { return 6; }

    let qi: quat = quat_new(1.0f, 0.0f, 0.0f, 0.0f);
    let qj: quat = quat_new(0.0f, 1.0f, 0.0f, 0.0f);
    let qk: quat = qi * qj;
    if (!nearly_equal(qk.quat_z(), 1.0f)) { return 7; }

    let scale2: mat4 = scale_matrix(2.0f, 2.0f, 2.0f);
    let scale3: mat4 = scale_matrix(3.0f, 3.0f, 3.0f);
    let scale6: mat4 = scale2 * scale3;
    if (!nearly_equal(mat4_get(scale6, 0, 0), 6.0f)) { return 8; }

    println("vector math checks passed");
    return 0;
}
```

Run it:

```console
build\ember_cli.exe run vector_math.ember --fn main --ffi
```

## What it demonstrates

- Vector `*` is component-wise. `v * v` therefore produces squared components, which `length_squared` sums.
- There is no `vec3 * f32` overload, so the velocity step is constructed component by component.
- Quaternion `*` is the Hamilton product: the basis identity `i * j = k` gives a `z` component of `1`.
- Matrix `*` is a true row-by-column product. Composing diagonal scales of 2 and 3 produces diagonal values of 6.
- Approximate comparison avoids demanding exact binary equality after floating-point arithmetic.
