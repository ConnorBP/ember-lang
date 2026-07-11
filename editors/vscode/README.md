# Ember — VS Code extension

Syntax highlighting, bracket matching, and comment toggling for the
**ember** language — a native-JIT embedded scripting language.

This is a declarative grammar-only extension: it contributes the `ember`
language id, the `.ember` file extension, a TextMate grammar, and a language
configuration. No runtime / language server is required, so nothing is
activated and no process is spawned — highlighting is provided by VS Code's
built-in grammar engine.

## Features

- Full keyword coverage matching `src/lexer.cpp`:
  - Declarations: `fn`, `struct`, `global`, `enum`, `let`, `mut`, `const`,
    `constexpr`, `defer`, `priv`, `link`
  - Control flow: `if`, `else`, `while`, `do`, `for`, `switch`, `case`,
    `default`, `break`, `continue`, `return`, `match`, `in` (for-each)
  - Cast / meta: `as`, `auto`, `sizeof`, `offsetof`
  - Primitives: `bool`, `i8`–`i64`, `u8`–`u64`, `f32`, `f64`, `void`
  - Literals: `true`, `false`
- Operators: `=>` (fat arrow), `->` (arrow), `::`, `..`, full
  comparison / arithmetic / bitwise / shift / compound-assignment set,
  `++`, `--`, `?`
- Comments: `//` line and `/* */` block
- Strings:
  - Plain `"..."` with escapes and `\`-line-continuation
  - f-strings `f"...{expr}..."` with `{...}` interpolation re-parsed as
    embedded ember expressions (incl. nested plain strings and `{{`/`}}`
    escaping)
  - Raw triple-quoted `r"""..."""` (literal, no escapes)
- Numbers: hex `0x..`, decimal, float (`1.0`, `1e5`, `1.0f`), and tolerant
  integer width suffixes (`42_u8` etc.) so suffix-typed literals still
  highlight as numbers even though the v1 lexer rejects them
- Annotations: `@entry`, `@on_tick`, `@obf("mba")`, …
- `struct`/`enum` names → type scope, `fn name` → function scope,
  `Name::Variant` → enum-variant scope

## Install

### From source (dev)

```bash
cd editors/vscode
npm install -g @vscode/vsce   # optional, for packaging
vsce package                  # produces ember-0.1.0.vsix
code --install-extension ember-0.1.0.vsix
```

Or, for development without packaging, symlink/copy this folder into your
`~/.vscode/extensions` directory, or open it in VS Code and press `F5`
(Launch Extension Development Host).

### Manually

Copy the `editors/vscode/` folder into `~/.vscode/extensions/ember/` and
restart VS Code.

## File layout

```
editors/vscode/
├── package.json                      # contributes: languages + grammars
├── language-configuration.json       # brackets, comments, autoclose, indent
├── syntaxes/ember.tmLanguage.json    # TextMate grammar
├── .vscodeignore                      # packaging excludes
└── README.md
```

## Keeping it in sync

The grammar is generated to track `src/lexer.cpp` and `src/lexer.hpp`. When
a new keyword / operator / literal form is added to the lexer, add it to the
corresponding repository entry here. The token inventory lives in
`enum class Tk` in `src/lexer.hpp`.
