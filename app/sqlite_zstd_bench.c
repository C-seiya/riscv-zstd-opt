/*
 * sqlite_zstd_bench.c — SQLite + Zstd 联合基准（RISC-V 第二框架适配）
 *
 * 功能：
 *   1. 生成压缩友好型 Blob 数据（模拟 KV 存储负载）
 *   2. 用 Zstd 压缩后写入 SQLite 表（框架二：SQLite 作为 KV 存储框架）
 *   3. 读回 → 解压 → CRC32 校验（数据完整性验证）
 *   4. 输出吞吐、压缩比、存储放大、完整性结果
 *
 * 用法: ./sqlite_zstd_bench [--db bench.db] [--rows 500] [--blob-size 4096]
 *                            [--level 3]
 *
 * 交叉编译（WSL）:
 *   riscv64-linux-gnu-gcc -O2 -static sqlite_zstd_bench.c sqlite3.o \
 *       -I<zstd>/include -L<zstd>/lib -lzstd -lpthread -ldl -lm \
 *       -o sqlite_zstd_bench
 * QEMU 运行:
 *   qemu-riscv64 -L /usr/riscv64-linux-gnu ./sqlite_zstd_bench \
 *       --db bench.db --rows 500 --blob-size 4096
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include <zstd.h>

/* ---------------- CRC32（自实现，避免额外依赖） ---------------- */
static uint32_t crc32_table[256];
static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
}
static uint32_t crc32_buf(const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = crc32_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---------------- 生成压缩友好型数据（模拟真实 KV 负载） ---------------- */
/* 结构：重复模板 + 行号变化，压缩率约 3~6x，与文本/JSON 日志负载特征一致 */
static void gen_blob(unsigned char *buf, size_t len, uint32_t row) {
    static const char *tpl =
        "{\"id\":%u,\"type\":\"sensor_reading\",\"payload\":\"aaaa"
        "bbbbbbbbbbbbbbbbbbbbccccccccccccccccddddddddddddddddddeee"
        "ffffffffffffffffggggggggggggggggghhhhhhhhhhhhhhhhhh\",\"ts\":%u}";
    size_t pos = 0;
    while (pos < len) {
        char tmp[256];
        int n = snprintf(tmp, sizeof tmp, tpl, row, row);
        if (n <= 0) break;
        size_t c = (size_t)n < (len - pos) ? (size_t)n : (len - pos);
        memcpy(buf + pos, tmp, c);
        pos += c;
    }
}

/* ---------------- 参数解析 ---------------- */
typedef struct {
    const char *db;
    int rows, blob_size, level;
} Config;

static Config parse_args(int argc, char **argv) {
    Config c = { "bench.db", 500, 4096, 3 };
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--db") && i + 1 < argc) c.db = argv[++i];
        else if (!strcmp(argv[i], "--rows") && i + 1 < argc) c.rows = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--blob-size") && i + 1 < argc) c.blob_size = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--level") && i + 1 < argc) c.level = atoi(argv[++i]);
    }
    return c;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    Config cfg = parse_args(argc, argv);
    crc32_init();

    sqlite3 *db = NULL;
    char *err = NULL;
    double t0, t1;
    long raw_bytes = 0, stored_bytes = 0;
    int failures = 0;

    printf("SQLite + Zstd 联合基准 (RISC-V 第二框架适配)\n");
    printf("  db=%s rows=%d blob_size=%d level=%d\n", cfg.db, cfg.rows, cfg.blob_size, cfg.level);
    printf("  sqlite %s | zstd %s\n\n", sqlite3_libversion(), ZSTD_versionString());

    /* 打开数据库 */
    if (sqlite3_open(cfg.db, &db) != SQLITE_OK) {
        fprintf(stderr, "无法打开数据库: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;", NULL, NULL, &err);

    /* 建表：哈希后的大小 + 压缩 blob */
    if (sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS kv("
        "  id INTEGER PRIMARY KEY,"
        "  crc INTEGER NOT NULL,"
        "  raw_len INTEGER NOT NULL,"
        "  comp_len INTEGER NOT NULL,"
        "  data BLOB NOT NULL);",
        NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "建表失败: %s\n", err ? err : "?");
        return 1;
    }

    sqlite3_stmt *ins = NULL, *sel = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO kv(id,crc,raw_len,comp_len,data) VALUES(?,?,?,?,?);", -1, &ins, NULL);
    sqlite3_prepare_v2(db,
        "SELECT crc,raw_len,comp_len,data FROM kv ORDER BY id;", -1, &sel, NULL);

    /* ---------- 阶段一：压缩 + 写入 ---------- */
    unsigned char *blob = malloc((size_t)cfg.blob_size);
    size_t ccap = ZSTD_compressBound((size_t)cfg.blob_size);
    unsigned char *cbuf = malloc(ccap);
    unsigned char *rbuf = malloc((size_t)cfg.blob_size);

    t0 = now_sec();
    for (int r = 1; r <= cfg.rows; r++) {
        gen_blob(blob, (size_t)cfg.blob_size, (uint32_t)r);
        uint32_t crc = crc32_buf(blob, (size_t)cfg.blob_size);
        size_t clen = ZSTD_compress(cbuf, ccap, blob, (size_t)cfg.blob_size, cfg.level);
        if (ZSTD_isError(clen)) {
            fprintf(stderr, "压缩失败: %s\n", ZSTD_getErrorName(clen));
            return 1;
        }
        raw_bytes += cfg.blob_size;
        stored_bytes += (long)clen;
        sqlite3_reset(ins);
        sqlite3_bind_int(ins, 1, r);
        sqlite3_bind_int64(ins, 2, (sqlite3_int64)crc);
        sqlite3_bind_int(ins, 3, cfg.blob_size);
        sqlite3_bind_int(ins, 4, (int)clen);
        sqlite3_bind_blob(ins, 5, cbuf, (int)clen, SQLITE_STATIC);
        if (sqlite3_step(ins) != SQLITE_DONE) {
            fprintf(stderr, "插入失败: %s\n", sqlite3_errmsg(db));
            return 1;
        }
    }
    t1 = now_sec();
    double wr_time = t1 - t0;
    double wr_mbps = wr_time > 0 ? (raw_bytes / 1048576.0) / wr_time : 0;

    /* ---------- 阶段二：读回 + 解压 + 校验 ---------- */
    t0 = now_sec();
    long read_rows = 0;
    while (sqlite3_step(sel) == SQLITE_ROW) {
        uint32_t crc_exp = (uint32_t)sqlite3_column_int64(sel, 0);
        int raw_len = sqlite3_column_int(sel, 1);
        const void *cdata = sqlite3_column_blob(sel, 3);
        int clen = sqlite3_column_int(sel, 2);
        if (raw_len > cfg.blob_size) { failures++; continue; }
        size_t rlen = ZSTD_decompress(rbuf, (size_t)raw_len, cdata, (size_t)clen);
        if (ZSTD_isError(rlen)) { failures++; continue; }
        if (crc32_buf(rbuf, rlen) != crc_exp) { failures++; continue; }
        read_rows++;
    }
    t1 = now_sec();
    double rd_time = t1 - t0;
    double rd_mbps = rd_time > 0 ? (raw_bytes / 1048576.0) / rd_time : 0;

    /* ---------- 报告 ---------- */
    printf("========== 基准结果 ==========\n");
    printf("写入行数        : %d\n", cfg.rows);
    printf("读回校验行数    : %ld (%s)\n", read_rows,
           failures == 0 ? "全部通过" : "存在失败");
    printf("原始数据总量    : %.2f MB\n", raw_bytes / 1048576.0);
    printf("压缩后存储总量  : %.2f MB\n", stored_bytes / 1048576.0);
    printf("压缩比          : %.3f\n", stored_bytes > 0 ? (double)raw_bytes / stored_bytes : 0);
    printf("存储放大(节省)  : %.1f%%\n",
           raw_bytes > 0 ? 100.0 * (1.0 - (double)stored_bytes / raw_bytes) : 0);
    printf("写入吞吐(含压缩): %.2f MB/s (QEMU 模拟值)\n", wr_mbps);
    printf("读回吞吐(含解压): %.2f MB/s (QEMU 模拟值)\n", rd_mbps);
    printf("数据完整性      : %s (%d 失败)\n", failures == 0 ? "PASS" : "FAIL", failures);
    printf("==============================\n");

    sqlite3_finalize(ins);
    sqlite3_finalize(sel);
    sqlite3_close(db);
    free(blob); free(cbuf); free(rbuf);
    return failures == 0 ? 0 : 2;
}
