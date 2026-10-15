/**
 * downloader.c - 断点续传下载核心
 *
 * 流程：
 *   1. (非续传) 先 sf_create() 建空洞文件，初始化 state
 *   2. 注册 SIGINT (Ctrl-C) 信号处理函数 —— 设置 interrupted 标志
 *   3. 循环：lseek + write 一个 CHUNK，更新 state.completed
 *   4. 每写 N 个块把 state 落盘（防止崩溃丢进度）
 *   5. SIGINT 到来：把 state 落盘，打印"新点"，优雅退出
 *   6. resume 子命令：load_state 后从 state.completed 续写
 */
#include "common.h"

/* 全局：信号处理函数只能写 volatile sig_atomic_t */
static volatile sig_atomic_t g_interrupted = 0;
static void sigint_handler(int sig)
{
    (void)sig;
    g_interrupted = 1;
}

/* 续传状态文件名 = 目标文件 + ".resume" */
static void make_state_path(char *buf, size_t buflen, const char *filename)
{
    snprintf(buf, buflen, "%s%s", filename, STATE_SUFFIX);
}

int dl_save_state(const char *filename, const download_state_t *st)
{
    char path[512];
    make_state_path(path, sizeof path, filename);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open state"); return -1; }

    /* 原子写：write 可能部分写，需要循环 */
    const char *p = (const char *)st;
    size_t left = sizeof *st;
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) { if (errno == EINTR) continue; perror("write state"); close(fd); return -1; }
        p += n; left -= (size_t)n;
    }
    /* fsync 保证落盘，避免 OS 页缓存丢数据 */
    fsync(fd);
    close(fd);
    return 0;
}

int dl_load_state(const char *filename, download_state_t *st)
{
    char path[512];
    make_state_path(path, sizeof path, filename);

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    ssize_t n = read(fd, st, sizeof *st);
    close(fd);
    if (n != (ssize_t)sizeof *st) return -1;

    /* 魔数校验 */
    if (memcmp(st->magic, "SPARSE-DL-V1", 12) != 0) {
        fprintf(stderr, "[err] 续传状态文件魔数不匹配，可能已损坏\n");
        return -1;
    }
    return 0;
}

int dl_clear_state(const char *filename)
{
    char path[512];
    make_state_path(path, sizeof path, filename);
    if (unlink(path) < 0 && errno != ENOENT) {
        perror("unlink state"); return -1;
    }
    return 0;
}

/* 进度条：每 5% 一格 */
static void progress_bar(off_t done, off_t total)
{
    static int last_cells = -1;
    int pct = (int)((double)done / total * 100.0);
    int cells = pct / 5;          /* 0~20 */
    if (cells == last_cells) return;
    last_cells = cells;

    putchar('\r');
    putchar('[');
    for (int i = 0; i < 20; i++) putchar(i < cells ? '#' : '-');
    printf("] %3d%%  %lld/%lld", pct, (long long)done, (long long)total);
    fflush(stdout);
}

/* 主下载循环 */
int dl_download(const char *filename, off_t total_size, int resume_flag)
{
    download_state_t state;
    memset(&state, 0, sizeof state);
    memcpy(state.magic, "SPARSE-DL-V1", 12);

    /* 1) 续传：尝试加载旧状态 */
    if (resume_flag) {
        if (dl_load_state(filename, &state) < 0) {
            printf("[info] 未找到合法续传状态，从头开始。\n");
            resume_flag = 0;
        } else {
            printf("[info] 📂 命中续传状态：已完成 %lld / %lld 字节\n",
                   (long long)state.completed, (long long)state.total_size);
            total_size = state.total_size;
        }
    }

    /* 2) 全新下载：先建空洞文件 + 初始化 state */
    if (!resume_flag) {
        if (sf_create(filename, total_size) < 0) return 1;
        strncpy(state.filename, filename, sizeof state.filename - 1);
        state.total_size = total_size;
        state.completed  = 0;
        state.start_time = time(NULL);
        state.chunk_idx  = 0;
    }
    state.last_update = time(NULL);

    /* 3) 注册 SIGINT：捕获 Ctrl-C，落盘后退出 */
    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags   = 0;            /* 不用 SA_RESTART，让 usleep/read 能被中断 */
    sigaction(SIGINT, &sa, &old_sa);
    g_interrupted = 0;

    /* 4) 打开目标文件准备定点写入 */
    int fd = open(filename, O_WRONLY);
    if (fd < 0) { perror("open"); return 1; }

    char chunk[CHUNK_SIZE];
    off_t offset = state.completed;
    unsigned saved_every = 0;     /* 每 4 个块落一次盘 */

    printf("[info] 🚀 开始下载 %s ...\n", filename);
    printf("[info]    (按 Ctrl-C 中断并保存断点，之后可用 `resume` 子命令续传)\n");
    progress_bar(offset, total_size);

    while (offset < total_size) {
        /* 被 Ctrl-C 中断：优雅退出 */
        if (g_interrupted) {
            printf("\n[info] ⏸  收到 SIGINT，保存续传状态...\n");
            state.completed = offset;
            state.last_update = time(NULL);
            dl_save_state(filename, &state);
            printf("[info]    ✅ 新点已保存: offset=%lld (%.2f%%)\n",
                   (long long)offset, (double)offset / total_size * 100.0);
            printf("[info]    使用 `./sparse-downloader resume %s` 继续\n", filename);
            close(fd);
            /* 恢复默认信号处理，避免下次调用异常 */
            sigaction(SIGINT, &old_sa, NULL);
            return 2;            /* 2 = 用户中断（非错误） */
        }

        size_t to_write = CHUNK_SIZE;
        if (offset + (off_t)to_write > total_size)
            to_write = (size_t)(total_size - offset);

        /* 模拟"下载到的内容"：用块号 + 偏移做填充字节，便于事后校验 */
        unsigned char fill = (unsigned char)(state.chunk_idx & 0xFF);
        memset(chunk, fill, to_write);

        /* lseek + write 定点写入 —— 不破坏其它空洞区 */
        if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
            perror("lseek"); break;
        }

        const char *p = chunk;
        size_t left = to_write;
        while (left > 0) {
            ssize_t n = write(fd, p, left);
            if (n < 0) {
                if (errno == EINTR) continue;
                perror("write"); break;
            }
            p += n; left -= (size_t)n;
        }
        if (left != 0) break;    /* 写入失败 */

        offset += (off_t)to_write;
        state.completed = offset;
        state.chunk_idx++;
        state.last_update = time(NULL);
        saved_every++;

        /* 每 4 个块落盘一次状态，崩溃可恢复 */
        if (saved_every >= 4) {
            dl_save_state(filename, &state);
            saved_every = 0;
        }

        progress_bar(offset, total_size);

        /* 50ms / 块 —— 让用户来得及按 Ctrl-C 演示断点 */
        usleep(50 * 1000);
    }

    printf("\n");
    close(fd);

    if (offset >= total_size) {
        printf("[ok]   ✅ 下载完成！共写入 %lld 字节，共 %u 个块\n",
               (long long)offset, state.chunk_idx);
        dl_clear_state(filename);
        sigaction(SIGINT, &old_sa, NULL);
        return 0;
    }

    /* 走到这里说明写入异常退出 */
    state.completed = offset;
    dl_save_state(filename, &state);
    sigaction(SIGINT, &old_sa, NULL);
    return 1;
}
