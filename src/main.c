/**
 * main.c - 命令行入口与命令分发
 *
 * 用法：
 *   sparse-downloader create   <file> <size>           生成空洞文件
 *   sparse-downloader write    <file> <offset> <hex>   定点写入（hex 形式）
 *   sparse-downloader download <file> [size]           启动下载（默认 256K）
 *   sparse-downloader resume   <file>                  从断点续传
 *   sparse-downloader status   <file>                  查看文件状态
 *   sparse-downloader verify   <file>                  校验下载内容一致性
 *   sparse-downloader help                            显示帮助
 */
#include "common.h"

static void usage(const char *prog)
{
    printf(
        "═══════════════════════════════════════════════════════════════\n"
        "  %s v%s  ——  空洞文件【新点续传】下载器\n"
        "  架构: 鲁班猫 RK3566 aarch64 (亦可在 x86_64 编译运行)\n"
        "  知识点: 文件 IO 与系统调用 (open/lseek/write/ftruncate/stat)\n"
        "═══════════════════════════════════════════════════════════════\n"
        "用法:\n"
        "  %s create   <file> <size>           生成空洞文件 (size 如 128K/1M)\n"
        "  %s write    <file> <offset> <hex>   在 offset 处写入 hex 字节序列\n"
        "  %s download <file> [size]           启动下载，默认 256K\n"
        "  %s resume   <file>                  从断点续传\n"
        "  %s status   <file>                  查看逻辑/物理大小与空洞占比\n"
        "  %s verify   <file>                  校验块填充字节是否一致\n"
        "  %s help                            显示本帮助\n"
        "═══════════════════════════════════════════════════════════════\n"
        "示例:\n"
        "  %s create test.bin 256K\n"
        "  %s write  test.bin 1024 deadbeef\n"
        "  %s download test.bin 256K\n"
        "    (在下载过程中按 Ctrl-C 中断)\n"
        "  %s resume   test.bin\n"
        "  %s status   test.bin\n"
        "  %s verify   test.bin\n",
        PROGRAM_NAME, PROGRAM_VERSION,
        prog, prog, prog, prog, prog, prog, prog,
        prog, prog, prog, prog, prog, prog);
}

/* 把形如 "deadbeef" 的 hex 字符串解析为字节数组，返回字节数 */
static int hex_to_bytes(const char *hex, unsigned char *out, int max)
{
    int n = 0;
    while (hex[0] && hex[1] && n < max) {
        unsigned int b;
        if (sscanf(hex, "%2x", &b) != 1) return -1;
        out[n++] = (unsigned char)b;
        hex += 2;
    }
    return n;
}

/* verify：检查每个 CHUNK 是否被填成了预期的 fill 字节 */
static int cmd_verify(const char *filename)
{
    struct stat st;
    if (stat(filename, &st) < 0) { perror("stat"); return 1; }

    int fd = open(filename, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }

    unsigned char buf[CHUNK_SIZE];
    off_t offset = 0;
    unsigned chunk_idx = 0;
    int bad = 0;

    while (offset < st.st_size) {
        size_t to_read = CHUNK_SIZE;
        if (offset + (off_t)to_read > st.st_size)
            to_read = (size_t)(st.st_size - offset);

        if (lseek(fd, offset, SEEK_SET) == (off_t)-1) { perror("lseek"); close(fd); return 1; }

        size_t got = 0;
        while (got < to_read) {
            ssize_t n = read(fd, buf + got, to_read - got);
            if (n <= 0) break;
            got += (size_t)n;
        }

        /* 期望填充字节 = 块号 & 0xFF；但只对"已下载区"校验，空洞区可能读到 0 */
        unsigned char expect = (unsigned char)(chunk_idx & 0xFF);
        int is_hole = 1;
        for (size_t i = 0; i < got; i++) {
            if (buf[i] != 0) { is_hole = 0; break; }
        }
        if (is_hole) {
            /* 全 0 —— 可能是空洞，跳过 */
        } else {
            for (size_t i = 0; i < got; i++) {
                if (buf[i] != expect) {
                    fprintf(stderr, "[fail] 块 %u 偏移 %lld: 期望 0x%02x, 实际 0x%02x\n",
                            chunk_idx, (long long)(offset + (off_t)i), expect, buf[i]);
                    bad++;
                    if (bad > 10) { printf("... (过多错误，停止)\n"); close(fd); return 1; }
                    break;
                }
            }
        }
        offset += (off_t)to_read;
        chunk_idx++;
    }
    close(fd);

    if (bad == 0) {
        printf("[ok]   ✅ 校验通过：所有已写入块的内容一致。\n");
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    const char *cmd = argv[1];

    if (!strcmp(cmd, "help") || !strcmp(cmd, "-h") || !strcmp(cmd, "--help")) {
        usage(argv[0]);
        return 0;
    }

    if (!strcmp(cmd, "create")) {
        if (argc < 4) { usage(argv[0]); return 1; }
        off_t sz = parse_size(argv[3]);
        if (sz <= 0) { fprintf(stderr, "[err] 非法 size: %s\n", argv[3]); return 1; }
        return sf_create(argv[2], sz) < 0 ? 1 : 0;
    }

    if (!strcmp(cmd, "write")) {
        if (argc < 5) { usage(argv[0]); return 1; }
        off_t off = (off_t)strtoll(argv[3], NULL, 0);
        unsigned char bytes[256];
        int n = hex_to_bytes(argv[4], bytes, (int)sizeof bytes);
        if (n <= 0) { fprintf(stderr, "[err] 非法 hex: %s\n", argv[4]); return 1; }
        int r = sf_write_at(argv[2], off, bytes, (size_t)n);
        if (r > 0) printf("[ok]   已写入 %d 字节到 %s @ offset %lld\n",
                          n, argv[2], (long long)off);
        return r > 0 ? 0 : 1;
    }

    if (!strcmp(cmd, "download")) {
        const char *fn = (argc > 2) ? argv[2] : DEFAULT_FILENAME;
        off_t sz = (argc > 3) ? parse_size(argv[3]) : DEFAULT_FILE_SIZE;
        if (sz <= 0) { fprintf(stderr, "[err] 非法 size\n"); return 1; }
        return dl_download(fn, sz, 0);
    }

    if (!strcmp(cmd, "resume")) {
        if (argc < 3) { usage(argv[0]); return 1; }
        return dl_download(argv[2], 0, 1);
    }

    if (!strcmp(cmd, "status")) {
        if (argc < 3) { usage(argv[0]); return 1; }
        return sf_status(argv[2]) < 0 ? 1 : 0;
    }

    if (!strcmp(cmd, "verify")) {
        if (argc < 3) { usage(argv[0]); return 1; }
        return cmd_verify(argv[2]);
    }

    fprintf(stderr, "[err] 未知命令: %s\n", cmd);
    usage(argv[0]);
    return 1;
}

