# Ember Developer Guide

Ember is a small, statically-typed scripting language that compiles to native code for embedding in host applications. It gives you fast, checked, near-native execution with a syntax deliberately kept close to C-family languages, plus a standard extension API surface (printing, assertions, string handling, math and vector types, arrays, and more) that every Ember host can expose into a running script.

> **NOTE:** This guide is for people writing `.ember` scripts against *any* host application that embeds Ember. It documents the language itself and the standard extension API that ships with Ember. It does not cover the Ember compiler internals, code generation, or how to embed the Ember toolchain into a new host application. Host-specific natives (a particular host's drawing, memory access, UI widgets, and so on) are documented by that host, not here.

## Who This Is For

If you are opening a `.ember` file, writing a script that calls the standard `print_*` / `string_*` / `vec*` / `array_*` natives, or wiring up an `@entry` or `@on_tick` handler, this documentation is for you. It assumes you can already read C-like code. It does not assume you know anything about how Ember is implemented.

## How the Guide Is Organized

The guide has three sections, meant to be used in different ways.

### Language Reference

The rules of the language itself: types, declarations, statements, operators, and the sharp edges worth knowing about before you hit them. Start with types, since almost everything else (function signatures, struct layout, casts, indexing) is stated in terms of them.

- [Types](10-language/10-types.md)

### API Reference

The standard extension natives every Ember host can expose to a running script: I/O and debug printing, assertions, string handling, math and vector types, and arrays. Each page documents one functional area. Start at the overview to see how the surface is organized. (Your host may expose additional, host-specific natives on top of these; see your host's documentation for those.)

- [API Overview](20-api/00-overview.md)

### Examples

Complete, working `.ember` scripts with a walkthrough of what each one does and why. Read these when you want to see idioms in context rather than in isolation. Fibonacci is the shortest and the best starting point.

- [Fibonacci](30-examples/10-fibonacci.md)

## Reading Order

New to Ember: read the Language Reference in order, skim the API Overview, then read the Fibonacci example end to end.

Already comfortable with the language: jump straight to the API Reference section you need, and treat the Examples as recipes.
