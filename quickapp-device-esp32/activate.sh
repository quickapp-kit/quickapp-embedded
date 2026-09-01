#!/usr/bin/env bash
# 统一激活 ESP-IDF 环境 (工具链在项目内 deps-source/espressif)。
# 用法:  source activate.sh
#
# 之后即可:  idf.py build  /  idf.py -p <PORT> flash monitor

_QA_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"

export IDF_TOOLS_PATH="$_QA_ROOT/deps-source/espressif"
export IDF_SKIP_CHECK_SUBMODULES=1

if [ ! -f "$_QA_ROOT/deps-source/esp-idf/export.sh" ]; then
  echo "ESP-IDF 未安装。先运行:  cd deps-source && ./setup.sh" >&2
  return 1 2>/dev/null || exit 1
fi

# shellcheck disable=SC1091
source "$_QA_ROOT/deps-source/esp-idf/export.sh"
