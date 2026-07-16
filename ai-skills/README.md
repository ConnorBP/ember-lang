# Ember AI skills

Prompt-time reference material for coding assistants working on Ember. Each
skill is a directory containing a `SKILL.md` with YAML frontmatter and a
Markdown body.

## Available skill

| Skill | Covers |
|---|---|
| [`ember-language`](ember-language/SKILL.md) | current Ember syntax, CLI, 17 extension libraries, all 25 built-in passes, VST3/node graphs, and self-hosting status |

## Install for pi

Pi discovers skills from `~/.pi/skills/`. Copy the directory, preserving the
`SKILL.md` name:

```bash
# Git Bash / MSYS2, Linux, or macOS
mkdir -p ~/.pi/skills
cp -r ai-skills/ember-language ~/.pi/skills/
```

Restart or reload the assistant session if the harness does not rescan skills
automatically.

The format is plain YAML frontmatter plus Markdown, so other assistants can use
the same file as a project instruction/reference document.

## Maintenance

Treat source and tests as authoritative. When Ember changes, update at least:

- lexer/parser keywords and examples;
- CLI options and supported toolchain;
- native/pass extension inventory;
- pass names and counts;
- VST3 callback contracts;
- self-hosting/parity status.

The skill targets the current `self-hosting-completion` branch rather than only
the latest tagged release.
