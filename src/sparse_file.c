/**
 * sparse_file.c - 空洞文件 (sparse file) 操作实现
 *
 * 三大核心能力：
 *   1. sf_create    —— 用 ftruncate 制造空洞文件 (未写入区不占磁盘块)
 *   2. sf_write_at  —— 用 lseek + write 定点写入，不破坏其余空闲区
 *   3. sf_status    —— 用 stat() 比对 st_size 与 st_blocks*512 得到空洞占比
 */
#include "common.h"

/* 1) create：生成空洞文件 ------------------------------------------------ */
int sf_create(const char *filename, off_t size)
{
    if (!filename || size <= 0) {
        errno = EINVAL;
        return -1;
    }
    ensure_min_size(size);

    /* O_WRONLY | O_CREAT | O_TRUNC：保证从零开始；0644 权限 */
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return -1; }

    /* ftruncate 是制造稀疏文件最干净的方式：
       文件系统只更新 inode 的 i_size，不分配数据块。 */
    if (ftruncate(fd, size) < 0) {
        perror("ftruncate");
        close(fd);
        return -1;
    }
    close(fd);

    char hbuf[32];
    printf("[ok]   空洞文件已创建: %s (逻辑大小 %lld 字节, %s)\n",
           filename, (long long)size, human_size(size, hbuf, sizeof hbuf));
    return 0;
}

/* 2) write：lseek + write 定点写入 -------------------------------------- */
int sf_write_at(const char *filename, off_t offset, const void *data, size_t len)
{
    if (!filename || !data || len == 0) {
        errno = EINVAL;
        return -1;
    }
    int fd = open(filename, O_WRONLY);
    if (fd < 0) { perror("open"); return -1; }

    /* lseek SEEK_SET 绝对偏移 —— "定点写入" 的关键 */
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
        perror("lseek"); close(fd); return -1;
    }

    /* 处理部分写：write 不保证一次写完 */
    const char *p = (const char *)data;
    size_t left = len;
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;   /* 被信号打断，重试 */
            perror("write"); close(fd); return -1;
        }
        p += n; left -= (size_t)n;
    }
    close(fd);
    return (int)len;
}

/* 辅助：定点读出，用于校验写入结果 */
int sf_read_at(const char *filename, off_t offset, void *buf, size_t len)
{
    int fd = open(filename, O_RDONLY);
    if (fd < 0) { perror("open"); return -1; }
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
        perror("lseek"); close(fd); return -1;
    }
    ssize_t n = read(fd, buf, len);
    close(fd);
    return (int)n;
}

/* 3) status：状态查询 --------------------------------------------------- */
int sf_status(const char *filename)
{
    struct stat st;
    if (stat(filename, &st) < 0) {
        perror("stat");
        return -1;
    }

    off_t     logical  = st.st_size;
    /* st_blocks 单位是 512 字节（POSIX 规定，与文件系统块大小无关） */
    off_t     physical = (off_t)st.st_blocks * 512;
    off_t     hole     = (logical > physical) ? (logical - physical) : 0;
    double    ratio    = logical > 0 ? (double)hole / logical * 100.0 : 0.0;

    char h_logical[32], h_physical[32], h_hole[32];

    printf("┌──────────────────────────────────────────────┐\n");
    printf("│  📊 文件状态: %-32s│\n", filename);
    printf("├──────────────────────────────────────────────┤\n");
    printf("│  逻辑大小   : %10lld 字节  (%s)\n",
           (long long)logical,  human_size(logical,  h_logical,  sizeof h_logical));
    printf("│  实际占用块 : %10llu 个     (每块 512B)\n",
           (unsigned long long)st.st_blocks);
    printf("│  物理大小   : %10lld 字节  (%s)\n",
           (long long)physical, human_size(physical, h_physical, sizeof h_physical));
    printf("│  空洞大小   : %10lld 字节  (%s)\n",
           (long long)hole,     human_size(hole,     h_hole,     sizeof h_hole));
    printf("│  空洞占比   : %10.2f %%\n", ratio);
    printf("│  块大小     : %10ld 字节\n", (long)st.st_blksize);
    printf("│  inode      : %10llu\n", (unsigned long long)st.st_ino);
    printf("└──────────────────────────────────────────────┘\n");

    /* 旁车 .resume 文件状态 */
    char state_file[512];
    snprintf(state_file, sizeof state_file, "%s%s", filename, STATE_SUFFIX);
    struct stat sst;
    if (stat(state_file, &sst) == 0) {
        printf("  ⏸  检测到续传状态文件: %s (%lld 字节)\n",
               state_file, (long long)sst.st_size);
    } else {
        printf("  ✅ 无续传状态文件（下载已完成或未开始）\n");
    }
    return 0;
}
