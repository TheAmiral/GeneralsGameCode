#!/usr/bin/env bash
# Armies Of Tomorrow: sets up the graphify code knowledge graph for Claude Code.
# Runs as a SessionStart hook. Safe to run by hand too: `bash scripts/graphify-setup.sh`.
# It never fails the session: every step is best-effort.

GRAPHIFY_VERSION="0.9.100"
ROOT="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$ROOT" || exit 0

# 1. Install the CLI if it is missing (local, free, no API key needed for code).
if ! command -v graphify >/dev/null 2>&1; then
  if command -v uv >/dev/null 2>&1; then
    uv tool install "graphifyy==${GRAPHIFY_VERSION}" >/dev/null 2>&1
  elif command -v pipx >/dev/null 2>&1; then
    pipx install "graphifyy==${GRAPHIFY_VERSION}" >/dev/null 2>&1
  fi
  export PATH="$HOME/.local/bin:$PATH"
fi
command -v graphify >/dev/null 2>&1 || { echo "graphify not installed (needs Python 3.10+ and uv or pipx)"; exit 0; }

# 2. Build or refresh the graph in the background (about 90 s from scratch on 4 cores,
#    a few seconds when only a few files changed). Scope is set by .graphifyignore.
LOG="graphify-out/setup.log"
mkdir -p graphify-out
if [ -f graphify-out/graph.json ]; then
  nohup graphify update . >"$LOG" 2>&1 &
  echo "graphify: refreshing graph in the background (log: $LOG)"
else
  nohup graphify extract . --code-only >"$LOG" 2>&1 &
  echo "graphify: building graph in the background, about 90 s (log: $LOG)"
fi
exit 0
