# Armies Of Tomorrow: WW3 engine fork

This is the Armies Of Tomorrow fork of the GeneralsGameCode engine. The game builds Zero Hour only (GeneralsMD + Core).

### graphify setup
- `scripts/graphify-setup.sh` runs at session start (see `.claude/settings.json`). It installs graphify if needed and builds the graph in the background in about 90 seconds. Wait for `graphify-out/graph.json` before relying on the rules below. Progress is in `graphify-out/setup.log`.
- The scope is set in `.graphifyignore`. The original `Generals/` tree is left out because it duplicates the Zero Hour symbols.
- The graph is code-only (local AST, no API key, no cost). It is not committed because it is about 90 MB.
- C++ class and method names often exist in several files. When `graphify explain` says a name is ambiguous, retry with `path::Symbol`.

### graphify rules

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

Rules:
- When graphify-out/graph.json exists, run `graphify query "<question>"` before your first grep, search or source-file read for a codebase question, and put this rule in every subagent prompt that explores code. Read raw files only for lines the graph or an earlier read already located. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
- A committed graphify-out/graph.json is rewritten whole on every build, so a large diff to it in a pull request is expected. Do not flag it as churn or suggest skipping the graph rebuild or its hooks.
