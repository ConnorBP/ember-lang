# ember — AI skills

Prompt-time skill files for AI coding assistants. Each subfolder is a
self-contained skill with a `SKILL.md` (YAML frontmatter + markdown body) that
teaches an assistant how to work with ember at the current version.

## Available skills

| Skill | Description |
|---|---|
| [`ember-language`](./ember-language/SKILL.md) | Write and debug `.ember` scripts: type system, syntax, CLI, passes, VST3, extensions, safety model |

## Installing into pi

pi loads skills from `~/.pi/skills/`. To install a skill, copy its folder:

```bash
# Windows (Git Bash / MSYS2)
cp -r ai-skills/ember-language ~/.pi/skills/

# Linux / macOS
cp -r ai-skills/ember-language ~/.pi/skills/
```

pi discovers the skill from the `SKILL.md` frontmatter (`name` + `description`)
and injects it when the assistant works on matching files.

## Using with other assistants

The `SKILL.md` format is plain YAML-frontmatter + markdown. Any AI assistant
that supports skill/instruction files can consume it directly — point the
assistant at the file, or paste its contents into a system prompt / custom
instruction.

## Updating

These skills are kept in sync with the current ember version. When the
language gains features (new passes, new extensions, syntax changes), update
the skill here and re-copy to `~/.pi/skills/`. The skill body documents the
version it targets (see the header in each `SKILL.md`).
