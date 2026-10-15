# ──────────────────────────────────────────────────────────────
# Makefile - 空洞文件【新点续传】下载器
# 架构/平台: 鲁班猫 RK3566 aarch64 (亦支持 x86_64)
# 注：使用 .RECIPEPREFIX 避免编辑器把 TAB 转空格的坑
# ──────────────────────────────────────────────────────────────

.RECIPEPREFIX := >

CC      ?= gcc
# 调试符号 + 严格警告（嵌入式 C 课程要求）
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -g -O2
LDFLAGS ?=

# 目标架构探测（鲁班猫 = aarch64；本机 = x86_64）
ARCH    := $(shell uname -m)

SRC_DIR  := src
OBJ_DIR  := obj
BIN      := sparse-downloader

SRCS     := $(wildcard $(SRC_DIR)/*.c)
OBJS     := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))

.PHONY: all build clean test run help info cross-aarch64

all: build

build: $(BIN)

$(BIN): $(OBJS)
>@echo "[link] $@  (arch=$(ARCH))"
>$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
>@echo "[cc]   $<"
>$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR):
>@mkdir -p $(OBJ_DIR)

# 快速运行示例：建文件 -> 看 status -> 下载 -> 中断 -> 续传 -> status
run: $(BIN)
>@./$(BIN) help
>@echo
>@./$(BIN) create demo.bin 256K
>@./$(BIN) status demo.bin
>@rm -f demo.bin

# 自动化测试：跑 tests/ 下的脚本
test: $(BIN)
>@echo "=== 运行测试用例 ==="
>@bash tests/run_all.sh ./$(BIN)
>@rm -f tests/*.out tests/*.bin tests/*.resume 2>/dev/null || true

# 交叉编译到 aarch64（鲁班猫平台），需安装 gcc-aarch64-linux-gnu
cross-aarch64:
>$(MAKE) CC=aarch64-linux-gnu-gcc CFLAGS="-std=c11 -Wall -Wextra -g -O2" build

info:
>@echo "ARCH       = $(ARCH)"
>@echo "CC         = $(CC)"
>@echo "CFLAGS     = $(CFLAGS)"
>@echo "BIN        = $(BIN)"
>@echo "SRCS       = $(SRCS)"

clean:
>@rm -rf $(OBJ_DIR) $(BIN) *.bin *.resume
>@echo "[clean] done"

help:
>@echo "可用目标:"
>@echo "  make build        编译生成 ./sparse-downloader"
>@echo "  make run          快速演示 (create + status)"
>@echo "  make test         运行测试套件 (tests/run_all.sh)"
>@echo "  make cross-aarch64  交叉编译到 RK3566 aarch64"
>@echo "  make info         打印构建变量"
>@echo "  make clean        清理产物"
