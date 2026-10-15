/**
 * common.h - 公共定义与类型声明
 *
 * 空洞文件【新点续传】下载器
 *   架构/平台 : 鲁班猫 | RK3566 | aarch64  (代码同样可在 x86_64 上编译运行)
 *   知识点     : 文件 IO 与系统调用 (open / lseek / write / ftruncate / stat / sigaction)
 *
 * 设计要点
 *   - 利用 ftruncate/lseek 制造空洞文件 (sparse file)，未写入区不占用磁盘块
 *   - 续传状态以二进制结构体持久化到 <file>.resume 旁车文件
 *   - SIGINT (Ctrl-C) 通过自描述 sigaction 捕获，落盘后优雅退出
 */
#ifndef COMMON_H
#define COMMON_H

/* 启用 POSIX 2008 扩展（sigaction / usleep / ftruncate 等） */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

#define PROGRAM_NAME        "sparse-downloader"
#define PROGRAM_VERSION     "1.0.0"

/* 作业要求：逻辑大小 >= 128KB */
#define MIN_SPARSE_SIZE     (128 * 1024)
#define DEFAULT_FILE_SIZE   (256 * 1024)   /* 256 KB */
#define CHUNK_SIZE          (4 * 1024)     /* 4 KB 下载块 */
#define STATE_SUFFIX        ".resume"      /* 续传状态文件后缀 */
#define DEFAULT_FILENAME    "download.bin"

/* 续传状态结构体 —— 持久化到磁盘 */
typedef struct {
    char     magic[16];        /* 魔数，校验文件合法性       */
    char     filename[256];    /* 目标文件名                  */
    off_t    total_size;       /* 逻辑总大小                  */
    off_t    completed;        /* 已完成偏移量 (新点)         */
    time_t   start_time;       /* 首次启动时间                */
    time_t   last_update;      /* 最后更新时间                */
    unsigned chunk_idx;        /* 当前块序号                  */
    unsigned reserved[4];      /* 预留字段，便于结构体演进    */
} download_state_t;

/* util.c */
off_t        parse_size(const char *s);
const char  *human_size(off_t bytes, char *buf, size_t buflen);
int          ensure_min_size(off_t size);

/* sparse_file.c */
int  sf_create(const char *filename, off_t size);
int  sf_write_at(const char *filename, off_t offset, const void *data, size_t len);
int  sf_read_at(const char *filename, off_t offset, void *buf, size_t len);
int  sf_status(const char *filename);

/* downloader.c */
int  dl_download(const char *filename, off_t total_size, int resume_flag);
int  dl_save_state(const char *filename, const download_state_t *st);
int  dl_load_state(const char *filename, download_state_t *st);
int  dl_clear_state(const char *filename);

#endif /* COMMON_H */
