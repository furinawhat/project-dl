/**
 * util.c - 工具函数实现
 */
#include "common.h"

/* 解析形如 "256K" / "1M" / "128KB" / "524288" 的大小字符串 */
off_t parse_size(const char *s)
{
    if (!s) return -1;
    char *end = NULL;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno != 0 || end == s) return -1;

    /* 跳过可选的 'B' */
    while (*end == ' ') end++;
    if (*end == 'B') end++;

    switch (*end) {
        case 'K': case 'k': v *= 1024;              end++; break;
        case 'M': case 'm': v *= 1024 * 1024;       end++; break;
        case 'G': case 'g': v *= 1024LL * 1024 * 1024; end++; break;
        case '\0': break;
        default: return -1;
    }
    return (off_t)v;
}

/* 把字节数格式化为人类可读形式: 1.00 KB / 2.50 MB */
const char *human_size(off_t bytes, char *buf, size_t buflen)
{
    double d = (double)bytes;
    if (d >= 1024.0 * 1024 * 1024)
        snprintf(buf, buflen, "%.2f GB", d / (1024.0 * 1024 * 1024));
    else if (d >= 1024.0 * 1024)
        snprintf(buf, buflen, "%.2f MB", d / (1024.0 * 1024));
    else if (d >= 1024.0)
        snprintf(buf, buflen, "%.2f KB", d / 1024.0);
    else
        snprintf(buf, buflen, "%lld B", (long long)bytes);
    return buf;
}

/* 校验尺寸是否满足作业要求 */
int ensure_min_size(off_t size)
{
    if (size < MIN_SPARSE_SIZE) {
        fprintf(stderr,
                "[warn] 逻辑大小 %lld 字节 < 作业要求 %d 字节 (128KB)。\n"
                "       将继续创建，但稀疏文件演示效果不明显。\n",
                (long long)size, MIN_SPARSE_SIZE);
        return -1;
    }
    return 0;
}
