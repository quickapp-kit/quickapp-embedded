#!/usr/bin/env bash
# 一键复现 ESP-IDF 构建环境 — 保证所有机器环境统一。
# 依据 DEPS.lock 中的精确版本, 将 ESP-IDF + 工具链安装到本目录 (deps-source/)。
#
# 用法:
#   cd deps-source && ./setup.sh
# 完成后激活:
#   source ../activate.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Avoid the outdated Apple system Python, whose package metadata handling is
# incompatible with current ESP-IDF transitive dependencies.
# shellcheck disable=SC1091
source ./select-python.sh

# 读取版本锁
# shellcheck disable=SC1091
source ./DEPS.lock

IDF_DIR="$SCRIPT_DIR/esp-idf"
TOOLS_DIR="$SCRIPT_DIR/espressif"

echo "==> 依赖将安装到: $SCRIPT_DIR"
echo "    ESP-IDF: $ESP_IDF_TAG ($ESP_IDF_COMMIT)"
echo "    目标芯片: $IDF_TARGET"

# 1. 获取 ESP-IDF 源码 (精确 commit)
if [ -d "$IDF_DIR/.git" ]; then
  echo "==> ESP-IDF 已存在, 校验/切换到锁定 commit ..."
  git -C "$IDF_DIR" fetch --depth 1 origin "$ESP_IDF_COMMIT" 2>/dev/null || \
    git -C "$IDF_DIR" fetch origin "$ESP_IDF_TAG"
  git -C "$IDF_DIR" checkout "$ESP_IDF_COMMIT" 2>/dev/null || \
    git -C "$IDF_DIR" checkout "$ESP_IDF_TAG"
else
  echo "==> 克隆 ESP-IDF ..."
  git clone -b "$ESP_IDF_TAG" "$ESP_IDF_REPO" "$IDF_DIR"
  git -C "$IDF_DIR" checkout "$ESP_IDF_COMMIT" 2>/dev/null || true
fi

# 2. 拉取子模块 (LVGL 无需, 但 IDF 系统组件如 mbedtls/nimble 等需要)
echo "==> 拉取 ESP-IDF 子模块 ..."
git -C "$IDF_DIR" submodule update --init --recursive --depth 1

# 3. 安装工具链到项目内 (IDF_TOOLS_PATH 指向 deps-source/espressif)
echo "==> 安装 $IDF_TARGET 工具链到 $TOOLS_DIR ..."
export IDF_TOOLS_PATH="$TOOLS_DIR"
"$IDF_DIR/install.sh" "$IDF_TARGET"

echo ""
echo "==> 完成。激活环境:"
echo "    source $(cd "$SCRIPT_DIR/.." && pwd)/activate.sh"
