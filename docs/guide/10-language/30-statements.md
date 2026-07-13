# Statements

This page is the full reference for every statement form in Ember: branching, loops, `switch`, `break`/`continue`, `return`, `defer`, expression statements, and block scoping rules. For a condensed list of the sharp edges called out below, see [50-gotchas.md](50-gotchas.md).

## if / else

```ember
if (health <= 0) {
    print_string("dead");
} else if (health < 25) {
    print_string("critical");
} else {
    print_string("ok");
}
```

The condition must be a `bool` expression. Ember has no "truthy" integers, there is no implicit conversion from `i64` (or any numeric type) to `bool`. Writing `if (count)` is a compile error, you must write `if (count != 0)`.

> **NOTE:** Every comparison operator (`==`, `!=`, `<`, `<=`, `>`, `>=`) requires both operands to be the same type and always produces a `bool`. There is no other way to produce a condition value.

The `else` branch is optional. `else if` is just an `else` whose body is another `if` statement, chained as deep as you like.

## while

```ember
let mut i: i64 = 0;
while (i < 10) {
    print_i64(i);
    i += 1;
}
```

The condition is checked before each iteration, including the first. If the condition is `false` on entry, the body never runs.

## do-while

```ember
let mut tries: i64 = 0;
do {
    tries += 1;
} while (tries < 3);
```

The body runs once before the condition is checked, so a `do-while` body always executes at least one time. Note the trailing semicolon after `while (cond)`: it is part of the statement's grammar and omitting it is a syntax error.

## for

```ember
for (let mut i: i64 = 0; i < 5; i += 1) {
    print_i64(i);
}
```

`for` is strictly C-style with three clauses (init, condition, step) separated by semicolons. **Ember also
ships a `for (x in slice)` / `for (x in array_handle)` for-each form** (see
`docs/spec/TYPE_SYSTEM.md` §13.2 + `CODEGEN_SPEC.md` §17) — the index-based
loop below is the manual alternative when you need the index:

```ember
fn sum_all(values: i64[], count: u64) -> i64 {
    let mut total: i64 = 0;
    for (let mut i: u64 = 0; i < count; i += 1) {
        total += values[i];
    }
    return total;
}
```

Internally `for` desugars to a `while` loop: the init clause runs once before the loop, the condition is checked before every iteration, and the step clause runs after the body on every iteration that actually executed (including one that ended in `continue`, see below). The loop variable declared in the init clause is scoped to the loop, it does not leak into the surrounding block.

Any of the three clauses may be omitted (an empty condition is treated as always true), but the two semicolons are always required:

```ember
for (;;) {
    break;
}
```

## switch

```ember
switch (kind) {
    case 0:
        print_string("none");
        break;
    case 1:
        print_string("player");
        break;
    case 2:
    case 3:
        print_string("npc or enemy");
        break;
    default:
        print_string("unknown");
        break;
}
```

Case labels must be literal constants, an integer literal, a character-width literal, whatever literal form the switch expression's type accepts. You cannot use a `const`-declared name as a case label even though its value is known at compile time.

### Fallthrough is real and is not checked

Ember's `switch` behaves exactly like C's: case bodies are emitted back to back with no jump between them, so execution falls through from one case into the next unless you end the case in `break`, `return`, or `continue`. This is *not* checked at compile time, an empty case that is meant to share a body with the next label (as `case 2:` does above, falling into `case 3:`) works for exactly the same reason a non-empty case without `break` also compiles and falls through, silently, into the next case's statements at runtime.

```ember
switch (x) {
    case 0:
        print_string("zero"); // no break: falls through and also prints "one" at runtime
    case 1:
        print_string("one");
        break;
}
```

> **WARNING:** There is no fallthrough keyword because none is needed, C-style fallthrough is simply what happens by default. There is also no compiler check that flags a missing `break`/`return`/`continue`, an accidentally-omitted `break` is a silent runtime behavior change, not a compile error. If you need shared behavior across cases, either stack the empty labels as shown above, add an explicit `break` at the end of every case that should not fall through, or extract the shared logic into its own function and call it from each case.

### The trailing-return-after-switch gotcha

Sema's return-path analysis deliberately does not treat a `switch` as exhaustive, even when every case returns and a `default` is present. A function whose body ends in nothing but a `switch` will fail to compile with a missing-return error unless you add a trailing `return` after the switch:

```ember
fn classify(x: i64) -> i64 {
    switch (x) {
        case 0:
            return 100;
        case 1:
            return 200;
        default:
            return 999;
    }
    // Still needed even though every case above returns:
    return 0;
}
```

This is a conservative, deliberate limitation, not a bug: sema does not attempt to prove that a `switch`'s cases cover every possible value of the switch expression's type, so it always requires a statement after the switch to guarantee the function returns on every path.

## break and continue

`break` exits the nearest enclosing `while`, `do-while`, `for`, or `switch`. `continue` skips to the next iteration of the nearest enclosing loop (`while`, `do-while`, or `for`); it is not valid to `continue` out of a `switch` toward a loop further out unless the `switch` itself is inside that loop, in which case it continues the loop, not the switch.

Inside a `for` loop, `continue` does not jump straight back to the top of the loop, it jumps to the step clause first, then the condition check:

```ember
for (let mut i: i64 = 0; i < 5; i += 1) {
    if (i == 2) {
        continue; // still runs i += 1, then rechecks i < 5
    }
    print_i64(i);
}
```

This matters if you are used to languages where `continue` skips the step clause too, in Ember it never does, the step always runs on every iteration that reaches the bottom of the loop body or hits `continue`.

## return

```ember
fn add(a: i64, b: i64) -> i64 {
    return a + b;
}

fn log_only(msg: string) {
    print_string(msg);
    return; // bare return is fine in a void function
}
```

A void function (no `-> rettype`, or an explicit `-> void`) uses a bare `return;` with no expression, or may omit a final `return` entirely and just fall off the end of the block. A value-returning function must return a value of the declared return type on every possible path through its body, sema checks this exhaustively and rejects any function where a path exists that does not end in a `return expr;`. This is what drives the trailing-return-after-switch requirement described above.

## defer

> **Also shipped (not documented in this guide):** `match (expr) { pat =>
> body, _ => default }` pattern matching (no fallthrough, integer/bool
> literals + `_` wildcard + **struct destructure patterns + guards** —
> `Point{x, y} if x > 0 =>`; see `docs/spec/CODEGEN_SPEC.md` §18), and
> `try { ... } catch (name) { ... }` / `throw expr;` in-language exceptions
> (see `docs/ROADMAP.md` Tier 4). This guide predates both.

```ember
fn demo() {
    defer print_string("first deferred, runs last");
    defer print_string("second deferred, runs first");
    print_string("body");
}
// Output order:
// body
// second deferred, runs first
// first deferred, runs last
```

`defer expr;` schedules `expr` to run when the enclosing block is exited, whether that exit happens by falling off the end of the block, by `return`, by `break`, or by `continue`. Multiple `defer` statements in the same block run in LIFO order: the most recently deferred expression runs first, unwinding back to the earliest one.

```ember
fn open_and_use() -> i64 {
    defer print_string("cleanup A");
    if (true) {
        defer print_string("cleanup B");
        return 1; // prints "cleanup B" then "cleanup A" before returning
    }
    return 0;
}
```

`defer` is scoped to the block it appears in: a `defer` inside an inner `{ }` block fires when that inner block is exited (including via `return`, `break`, or `continue` out of it), not only when the whole function ends.

> **WARNING:** `defer` does not run on a trap or abort-unwind. If the deferred cleanup is something that must happen even when the script hits a runtime trap (a failed `assert_eq_*`, a divide-by-zero, and so on), do not rely on `defer` alone, this is a deliberate v1 limitation, not an oversight. `defer` only guarantees cleanup on the normal control-flow exits: fallthrough, `return`, `break`, and `continue`. Note that an out-of-bounds array or slice index **does** trap (`TrapReason::BoundsCheck`), so it is one of the cases where pending `defer`s do not run — the trap unwinds without running deferred cleanup, exactly like a divide-by-zero or a failed assertion. Indexing is bounds-checked at runtime (and at compile time for literal constant indices); see [40-expressions-operators.md](40-expressions-operators.md) §Indexing.

## Expression statements

Any expression followed by a semicolon is a valid statement on its own: function calls, assignments, and increment/decrement are the common cases.

```ember
log_u64(42);
x = y + 1;
counter += 1;
arr[i]++;
```

The expression's value (if it produces one) is discarded. This is normal for calls made for their side effects and for assignment/compound-assignment forms.

## Block scoping

Every `{ }` block opens a new scope, this applies to function bodies, `if`/`else` bodies, loop bodies, and any bare `{ }` block written on its own. A name declared inside a block (with `let`, `let mut`, `auto`, or `const`) is only visible from its declaration point to the end of that block.

```ember
let x: i64 = 1;
{
    let x: i64 = 2; // shadows the outer x, allowed
    print_i64(x);   // 2
}
print_i64(x); // 1
```

Shadowing a name from an outer scope is allowed, an inner block may redeclare a name that already exists in a scope that encloses it. What is not allowed is redeclaring the same name twice in the same block:

```ember
let mut y: i64 = 0;
let y: i64 = 1; // compile error: y already declared in this block
```

Function parameters live in a scope that encloses the function body block, so a parameter name can be shadowed by a `let` inside the body the same way an outer local can be, but a parameter cannot share its name with another parameter in the same parameter list.

---

For the consolidated list of statement-related pitfalls (switch fallthrough surprises, the trailing-return-after-switch requirement, `defer` and traps, and more), see [50-gotchas.md](50-gotchas.md).
