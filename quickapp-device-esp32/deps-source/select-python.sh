#!/usr/bin/env bash

qa_select_idf_python() {
  local candidate
  local detected_python

  detected_python="$(command -v python3 2>/dev/null || true)"
  for candidate in \
    "${QA_IDF_PYTHON:-}" \
    /opt/homebrew/bin/python3 \
    /usr/local/bin/python3 \
    "$detected_python"; do
    if [ -n "$candidate" ] && [ -x "$candidate" ] && \
      "$candidate" -c 'import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)' 2>/dev/null; then
      export PATH="$(dirname "$candidate"):$PATH"
      return 0
    fi
  done

  echo "ESP-IDF requires Python 3.10+. Set QA_IDF_PYTHON to a compatible interpreter." >&2
  return 1
}

qa_select_idf_python
unset -f qa_select_idf_python
