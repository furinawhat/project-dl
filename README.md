# 空洞文件【新点续传】下载器

> Linux 嵌入式系统课程 · 文件 IO 编程作业（AI 协作版）
>
> **平台**：鲁班猫 RK3566 aarch64（亦可在 x86_64 Linux 编译运行）
> **知识点**：文件 IO 与系统调用（`open` / `lseek` / `write` / `ftruncate` / `stat` / `sigaction`）
> **AI 工具链**：DeepSeek API + Claude Code（详见 `docs/AI协作日志.md`）

---

## 一、项目简介

本项目实现一个**基于空洞文件（sparse file）的"新点续传"下载器**。

核心思想：
1. 用 `ftruncate()` 预先创建一个逻辑大小 ≥ 128 KB 的**空洞文件**，未写入区不占磁盘块；
2. 用 `lseek()` + `write()` 进行**定点写入**，模拟分块下载，不破坏其余空闲区；
3. 用 `sigaction()` 捕获 `SIGINT`（Ctrl-C），把"已完成偏移量"持久化到 `.resume` 旁车文件；
4. 下次 `resume` 时读取旁车文件，从**新点（断点）**继续写入；
5. `status` 子命令通过 `stat()` 比较 `st_size` 与 `st_blocks*512` 计算空洞占比。

---

## 二、目录结构

```
sparse-downloader/
├── Makefile                  # 构建脚本 (build/run/test/cross-aarch64/clean)
├── README.md                 # 本文件
├── src/                      # 源码
│   ├── common.h              # 公共头文件 + 类型定义 + 宏
│   ├── util.c                # 工具函数 (parse_size / human_size)
│   ├── sparse_file.c         # 空洞文件操作 (create / write_at / status)
│   ├── downloader.c          # 下载核心 (SIGINT / 状态持久化 / 续传)
│   └── main.c                # 命令行入口与分发
├── tests/
│   └── run_all.sh            # 自动化测试脚本 (17 项测试)
└── docs/
    ├── AI协作日志.md          # AI 提示词 + 采纳/拒绝记录
    ├── AI代码审查.md          # ≥3 处缺陷 + 现象 + 根因 + 修复
    ├── 测试用例.md            # AI 辅助测试用例
    └── 运行截图.txt           # 端到端运行输出截图
```

---

## 三、编译

### 3.1 本机编译（x86_64）

```bash
make build
```

产物：`./sparse-downloader`

编译选项：`-std=c11 -Wall -Wextra -Wpedantic -g -O2`（零警告通过）

### 3.2 交叉编译到鲁班猫 RK3566（aarch64）

```bash
# 需先安装交叉编译工具链
sudo apt install gcc-aarch64-linux-gnu
make cross-aarch64
```

把生成的 `sparse-downloader` 通过 `scp` 拷贝到鲁班猫板子上即可运行（无外部依赖）。

### 3.3 清理

```bash
make clean
```

---

## 四、用法

### 4.1 命令一览

```
sparse-downloader create   <file> <size>           生成空洞文件 (size 如 128K/1M)
sparse-downloader write    <file> <offset> <hex>   在 offset 处写入 hex 字节序列
sparse-downloader download <file> [size]           启动下载，默认 256K
sparse-downloader resume   <file>                  从断点续传
sparse-downloader status   <file>                  查看逻辑/物理大小与空洞占比
sparse-downloader verify   <file>                  校验块填充字节是否一致
sparse-downloader help                            显示帮助
```

### 4.2 典型工作流（断点续传演示）

```bash
# 1) 启动下载（256 KB）
./sparse-downloader download demo.bin 256K
#    输出进度条，过程中按 Ctrl-C 中断
#    → [info] ✅ 新点已保存: offset=163840 (62.50%)

# 2) 查看中断后的状态
./sparse-downloader status demo.bin
#    空洞占比: 37.50 %    （已下载 160KB 实占，剩 96KB 空洞）
#    ⏸  检测到续传状态文件: demo.bin.resume

# 3) 从断点续传
./sparse-downloader resume demo.bin
#    [info] 📂 命中续传状态：已完成 163840 / 262144 字节
#    [ok]   ✅ 下载完成！共写入 262144 字节，共 64 个块

# 4) 查看最终状态
./sparse-downloader status demo.bin
#    空洞占比: 0.00 %    （全部已下载，无空洞）

# 5) 校验数据一致性
./sparse-downloader verify demo.bin
#    [ok]   ✅ 校验通过
```

### 4.3 单独测试空洞文件

```bash
# 创建 256KB 空洞文件
./sparse-downloader create test.bin 256K
./sparse-downloader status test.bin
#   实际占用块: 0 个   空洞占比: 100.00 %

# 定点写入（不破坏其余空洞）
./sparse-downloader write test.bin 1024 deadbeef
./sparse-downloader write test.bin 131072 cafebabe

# 验证写入位置
od -A x -t x1 -j 1024 -N 4 test.bin        # → de ad be ef
od -A x -t x1 -j 131072 -N 4 test.bin      # → ca fe ba be

./sparse-downloader status test.bin
#   实际占用块: 16 个 (8KB)   空洞占比: 96.88 %
```

---

## 五、技术原理

### 5.1 空洞文件 (Sparse File) 是什么

Linux 文件系统中，一个文件由 **inode**（元数据）+ 若干**数据块**组成。
- 普通文件：每段逻辑偏移都有对应的数据块。
- **空洞文件**：部分逻辑偏移没有数据块，读这些位置返回 `\0`，但**不占磁盘空间**。

产生空洞的方式：
| 方式 | 说明 |
| --- | --- |
| `ftruncate(fd, size)` | 把文件逻辑大小直接扩展到 `size`，不分配数据块。**最干净**。 |
| `lseek(fd, size-1, SEEK_SET); write(fd, "", 1)` | 跳过中间区域，在末尾写 1 字节。中间区为空洞。 |

本项目用 `ftruncate()` 创建初始空洞文件。

### 5.2 status 命令的计算公式

```c
struct stat st;
stat(filename, &st);

off_t  logical  = st.st_size;          // 逻辑大小
off_t  physical = st.st_blocks * 512;  // 物理大小（st_blocks 单位固定 512B）
off_t  hole     = logical - physical;
double ratio    = (double)hole / logical * 100;
```

> ⚠️ 注意：POSIX 规定 `st_blocks` 的单位**永远是 512 字节**，与 `st_blksize`（文件系统块大小，通常 4KB）无关。这是初学者常踩的坑（见 `docs/AI代码审查.md` 缺陷 #2）。

### 5.3 断点续传的状态设计

```
download_state_t {
    magic[16]        // "SPARSE-DL-V1"  —— 校验文件合法性
    filename[256]    // 目标文件名
    total_size       // 逻辑总大小
    completed        // 已完成偏移量（新点）
    start_time       // 首次启动时间
    last_update      // 最后更新时间
    chunk_idx        // 当前块序号
    reserved[4]      // 预留字段，便于结构体演进
}
```

持久化到 `<file>.resume`，每 4 个块 + 收到 `SIGINT` 时各落盘一次（`fsync` 保证刷盘）。

### 5.4 SIGINT 信号处理

```c
static volatile sig_atomic_t g_interrupted = 0;

static void sigint_handler(int sig) {
    (void)sig;
    g_interrupted = 1;          // 只设置标志，不调用非异步信号安全函数
}

// 主循环里检查：
while (offset < total_size) {
    if (g_interrupted) {
        // 保存状态，优雅退出
        dl_save_state(...);
        return 2;
    }
    // ... 写一个块 ...
}
```

**关键**：信号处理函数里**不**直接调用 `printf`/`write`/`save_state`（这些不是异步信号安全的），
只设置 `volatile sig_atomic_t` 标志，由主循环检测后做安全清理。

---

## 六、测试

```bash
make test
```

测试套件覆盖 17 项：
- help 命令
- create 空洞文件（验证逻辑大小 + 实际块=0）
- write 定点写入（不破坏空洞 + 内容正确）
- status 输出
- download 全流程
- verify 校验
- **断点续传**（中断 → resume → 状态清理 → 数据一致）
- 非法参数处理

详见 `docs/测试用例.md`。

---

## 七、AI 协作

本项目按作业要求融入 5 项 AI 元素，详见：

| 文档 | 内容 |
| --- | --- |
| `docs/AI协作日志.md` | ≥5 条 DeepSeek/Claude 提示词 + 采纳/拒绝决策 |
| `docs/AI代码审查.md` | ≥3 处缺陷：现象 + 根因 + 修复 |
| `docs/测试用例.md` | AI 辅助生成的测试用例 |
| `docs/运行截图.txt` | 端到端运行输出 |
| `README.md` | 本文件（AI 起草 + 学生核对） |

---

## 八、平台兼容性

| 平台 | 状态 | 说明 |
| --- | --- | --- |
| x86_64 Linux | ✅ 已验证 | 本机开发与测试环境 |
| aarch64 (RK3566 鲁班猫) | ✅ 可交叉编译 | `make cross-aarch64` |
| macOS / FreeBSD | ⚠️ 未测试 | `ftruncate`/`stat` 接口兼容，但 `st_blocks` 语义相同 |

代码仅使用 POSIX.1-2008 标准接口，无平台特定依赖。

---

## 九、已知限制

1. 下载内容为**模拟数据**（用块号做填充字节便于 `verify`），未对接真实 HTTP。
2. `write` 子命令的 hex 参数最长 256 字节（足够演示）。
3. 续传状态文件未加密，生产环境应考虑校验和/加密。

---

## 十、作者

学生姓名：__________  学号：__________  班级：__________

提交时间：第 7 周周五上课前
