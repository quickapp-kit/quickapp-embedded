# deps-source — 构建依赖

本目录存放 ESP-IDF 及其工具链。**实际二进制不进 git**（约 4GB, 平台相关），
通过 `setup.sh` + `DEPS.lock` 精确复现, 保证所有机器环境统一。

## 目录结构

```
deps-source/
├── setup.sh        # 一键安装脚本 (提交)
├── DEPS.lock       # 版本锁: ESP-IDF commit + 目标芯片 (提交)
├── README.md       # 本文件 (提交)
├── esp-idf/        # ESP-IDF v5.4 源码 (不提交, setup.sh 生成)
└── espressif/      # 工具链: 交叉编译器 + python venv (不提交, setup.sh 生成)
```

## 首次安装 (换新机器 / 重装)

```bash
cd deps-source
./setup.sh          # 按 DEPS.lock 精确复现, 约 10 分钟 (取决于网速)
```

## 日常使用

```bash
# 在项目根目录
source activate.sh                       # 激活环境
idf.py build                             # 编译
idf.py -p /dev/cu.usbserial-1130 flash monitor   # 烧录 + 串口监视
```

## 环境统一的保证

- **ESP-IDF 源码**: `DEPS.lock` 锁定到精确 commit, `setup.sh` checkout 到该点。
- **工具链**: 由 ESP-IDF 自带的 `tools.json` 精确锁定补丁版本, `install.sh` 按目标芯片安装。
- **Python 依赖**: `install.sh` 建独立 venv, 按 `espidf.constraints.v5.4.txt` 精确装。
- **项目配置**: `sdkconfig.defaults` (提交) 是唯一配置源, `sdkconfig` (不提交) 由它生成。

任意机器跑一次 `setup.sh` 即得到逐字节一致的构建环境。
