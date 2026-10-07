/*
 * analysis_v2.c — RISC-V 指令混合度分析工具（v2）
 *
 * 功能：
 *   1. 单日志模式：统计 QEMU -d in_asm 轨迹的指令类别分布
 *      用法: ./analysis_v2 single <log> [-o out.md] [-t]
 *   2. 双日志 diff 模式：对比优化前后两份日志，输出 Markdown 对比报告
 *      用法: ./analysis_v2 diff <base.log> <opt.log> [-o out.md] [-t]
 *
 * 指令分类（六类，向量类为 RVV 预备，仅非零时显示）：
 *   arith  — 算术/逻辑/比较/乘除/浮点运算
 *   mem    — 访存（load/store/AMO）
 *   branch — 条件分支
 *   jump   — 无条件跳转（jal/jalr 及压缩形式）
 *   vector — RVV 向量指令（v*.vv / vl* / vs* 等）
 *   other  — 系统指令（ecall/ebreak/fence/csr 访问/nop 等）
 *
 * 编译: gcc -O2 -o analysis_v2 analysis_v2.c
 * 说明: 计数语义与 v1 (analysis.c) 保持一致——统计 -d in_asm 反汇编出的
 *       全部指令行（翻译块级轨迹），保证与既有基线数据可比。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define NCAT 6
#define MAX_MNEM 4096
#define LINE_LEN 1024

static const char *CAT_NAME[NCAT] = {"arith", "mem", "branch", "jump", "vector", "other"};

typedef struct {
    long total;
    long cat[NCAT];
    /* 助记符热点统计（简单哈希表） */
    char  mnem[MAX_MNEM][16];
    long  mnem_count[MAX_MNEM];
    int   mnem_used;
} Stats;

/* ---------------- 指令分类表 ---------------- */

/* 访存指令前缀/全名 */
static const char *MEM_LIST[] = {
    "lw","lh","lhu","lb","lbu","lwu","ld","lr","sc",
    "sw","sh","sb","sd",
    "flw","fld","flq","fsw","fsd","fsq",
    "amo","amoswap","amoadd","amoxor","amoand","amoor","amomin","amomax",
    "amominu","amomaxu",
    "c.lw","c.ld","c.lwsp","c.ldsp","c.sw","c.sd","c.swsp","c.sdsp",
    "c.flw","c.fld","c.flwsp","c.fldsp","c.fsw","c.fsd","c.fswsp","c.fsdsp",
    NULL
};
/* 分支指令 */
static const char *BR_LIST[] = {
    "beq","bne","blt","bge","bltu","bgeu",
    "c.beqz","c.bnez", NULL
};
/* 跳转指令 */
static const char *JMP_LIST[] = {
    "jal","jalr","j","jr","call","tail","ret",
    "c.j","c.jal","c.jr","c.jalr", NULL
};
/* 算术/逻辑（含前缀匹配列表） */
static const char *ARITH_PREFIX[] = {
    "add","sub","and","or","xor","slt","sltu","sll","srl","sra",
    "mul","div","rem","seqz","snez","sltz","sgtz","neg","not",
    "sext","zext","mv","li","lui","auipc",
    "fadd","fsub","fmul","fdiv","fsqrt","fcvt","fmv","fsgnj","fmin","fmax",
    "feq","flt","fle","fclass","fneg","fabs","fmadd","fmsub","fnmadd","fnmsub",
    "csrr","csrw","csrs","csrc","csrrw","csrrs","csrrc","csrrwi","csrrsi","csrrci",
    "rdcycle","rdtime","rdinstret",
    "sh1add","sh2add","sh3add","andn","orn","xnor","clz","ctz","cpop",
    "min","max","minu","maxu","rol","ror",
    NULL
};
/* other：ecall ebreak ebreak1 fence fence.i wfi nop mv? (mv 归 arith) pause */

static int in_list(const char *m, const char **list) {
    for (int i = 0; list[i]; i++) if (!strcmp(m, list[i])) return 1;
    return 0;
}
static int prefix_match(const char *m, const char **list) {
    for (int i = 0; list[i]; i++) {
        size_t l = strlen(list[i]);
        if (!strncmp(m, list[i], l)) {
            char c = m[l];
            /* 前缀后必须跟 '.' 或 'w'/'i' 等变体字符或结尾 */
            if (c == '\0' || c == '.' || c == 'w' || c == 'i' || c == 'u') return 1;
        }
    }
    return 0;
}

static int classify(const char *m) {
    size_t l = strlen(m);
    if (l == 0) return -1;
    /* RVV 向量：v 开头且为已知向量助记符族 */
    if ((m[0] == 'v' && l >= 2 && strchr("./_", m[1]) == NULL &&
         strstr(m, "vset") != m && strncmp(m, "vn", 2) != 0) ||
        !strncmp(m, "vset", 4) || !strncmp(m, "vl", 2) || !strncmp(m, "vs", 2)) {
        /* 排除与向量无关的助记符误报：如 vs 之外的常规指令极少以 v 开头 */
        if (m[0] == 'v' && l >= 2 && m[1] != '.') {
            /* vadd.vv, vmul.vx, vlxei8.v, vsetvli, vle32.v ... 全部归 vector */
            return 4;
        }
    }
    if (in_list(m, MEM_LIST))   return 1;
    if (in_list(m, BR_LIST))    return 2;
    if (in_list(m, JMP_LIST))   return 3;
    if (prefix_match(m, ARITH_PREFIX)) return 0;
    return 5; /* other */
}

/* ---------------- 统计逻辑 ---------------- */

static void mnem_add(Stats *s, const char *m) {
    for (int i = 0; i < s->mnem_used; i++) {
        if (!strcmp(s->mnem[i], m)) { s->mnem_count[i]++; return; }
    }
    if (s->mnem_used < MAX_MNEM) {
        strncpy(s->mnem[s->mnem_used], m, 15);
        s->mnem[s->mnem_used][15] = '\0';
        s->mnem_count[s->mnem_used] = 1;
        s->mnem_used++;
    }
}

/* 从一行中提取助记符。
 * QEMU in_asm 行格式: "0x0000000000010464:  00050513  addi a0,a0,0"
 * 兼容无地址列的格式（旧 QEMU/裁剪日志）。
 */
static int extract_mnemonic(const char *line, char *mnem, size_t sz) {
    const char *p = line;
    char tok[64];
    int idx = 0;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        int n = 0;
        while (*p && !isspace((unsigned char)*p) && n < 63) tok[n++] = *p++;
        tok[n] = '\0';
        idx++;
        if (idx == 1) {
            /* 应为地址（0x 开头）；若不是则该行非指令行 */
            if (strncmp(tok, "0x", 2) != 0) return 0;
        } else if (idx == 2) {
            /* 应为十六进制操作码（全 hex） */
            for (int i = 0; tok[i]; i++)
                if (!isxdigit((unsigned char)tok[i])) return 0;
        } else if (idx == 3) {
            /* 助记符：去掉尾部逗号 */
            char *q = tok;
            char *e = q + strlen(q) - 1;
            while (e >= q && (*e == ',')) *e-- = '\0';
            snprintf(mnem, sz, "%s", q);
            return 1;
        }
    }
    return 0;
}

static void analyze_file(const char *path, Stats *s) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "无法打开 %s\n", path); exit(1); }
    char line[LINE_LEN], mnem[32];
    long lines = 0;
    while (fgets(line, LINE_LEN, f)) {
        lines++;
        /* 跳过非指令行 */
        if (line[0] == '-' || line[0] == 'I' || line[0] == 'T') continue;
        if (strstr(line, "IN:") || strstr(line, "Trace")) continue;
        if (extract_mnemonic(line, mnem, sizeof mnem)) {
            int c = classify(mnem);
            if (c >= 0) {
                s->total++;
                s->cat[c]++;
                mnem_add(s, mnem);
            }
        }
    }
    fclose(f);
    fprintf(stderr, "[info] %s: 读取 %ld 行\n", path, lines);
}

/* ---------------- 输出 ---------------- */

static const char *pct(double v, char *buf, size_t sz) {
    snprintf(buf, sz, "%.2f%%", v);
    return buf;
}

static void print_single(FILE *out, const char *name, Stats *s) {
    char b[16];
    fprintf(out, "## 指令混合度统计：%s\n\n", name);
    fprintf(out, "| 类别 | 数量 | 占比 |\n|---|---|---|\n");
    fprintf(out, "| **总计** | **%ld** | 100%% |\n", s->total);
    for (int i = 0; i < NCAT; i++) {
        if (s->cat[i] == 0 && i == 4) continue; /* 向量类为零时不显示 */
        double p = s->total ? 100.0 * s->cat[i] / s->total : 0;
        fprintf(out, "| %s | %ld | %s |\n", CAT_NAME[i], s->cat[i], pct(p, b, sizeof b));
    }
    /* 热点 Top15 */
    fprintf(out, "\n### 热点助记符 Top 15\n\n");
    fprintf(out, "| 助记符 | 次数 | 占比 |\n|---|---|---|\n");
    int idx[MAX_MNEM];
    for (int i = 0; i < s->mnem_used; i++) idx[i] = i;
    /* 简单选择排序（15 个足够） */
    for (int i = 0; i < s->mnem_used && i < 15; i++) {
        int max = i;
        for (int j = i + 1; j < s->mnem_used; j++)
            if (s->mnem_count[idx[j]] > s->mnem_count[idx[max]]) max = j;
        int t = idx[i]; idx[i] = idx[max]; idx[max] = t;
        fprintf(out, "| %s | %ld | %s |\n", s->mnem[idx[i]], s->mnem_count[idx[i]],
                pct(100.0 * s->mnem_count[idx[i]] / (s->total ? s->total : 1), b, sizeof b));
    }
}

static void print_diff(FILE *out, const char *bname, const char *oname, Stats *b, Stats *o) {
    char buf[16];
    fprintf(out, "# 优化前后指令混合度对比\n\n");
    fprintf(out, "- 基线日志：`%s`\n- 优化日志：`%s`\n\n", bname, oname);
    fprintf(out, "| 类别 | 优化前 | 占比 | 优化后 | 占比 | 变化 |\n|---|---|---|---|---|---|\n");
    long dt = o->total - b->total;
    fprintf(out, "| **总计** | **%ld** | 100%% | **%ld** | 100%% | **%+ld (%+.2f%%)** |\n",
            b->total, o->total, dt, b->total ? 100.0 * dt / b->total : 0);
    for (int i = 0; i < NCAT; i++) {
        if (b->cat[i] == 0 && o->cat[i] == 0 && i == 4) continue;
        double pb = b->total ? 100.0 * b->cat[i] / b->total : 0;
        double po = o->total ? 100.0 * o->cat[i] / o->total : 0;
        long d = o->cat[i] - b->cat[i];
        fprintf(out, "| %s | %ld | %s | %ld | %s | %+ld |\n",
                CAT_NAME[i], b->cat[i], pct(pb, buf, sizeof buf),
                o->cat[i], pct(po, buf, sizeof buf), d);
    }
    fprintf(out, "\n> 由 analysis_v2 自动生成，可直接嵌入参赛文档。\n");
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
            "用法:\n"
            "  %s single <log>      [-o out.md] [-t]  单日志统计\n"
            "  %s diff <base> <opt> [-o out.md] [-t]  双日志对比\n"
            "  -t 同时输出到终端\n", argv[0], argv[0]);
        return 1;
    }
    const char *mode = argv[1];
    const char *out_path = NULL;
    int to_term = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "-t")) to_term = 1;
    }

    Stats s1 = {0}, s2 = {0};
    FILE *out = stdout;

    if (!strcmp(mode, "single")) {
        analyze_file(argv[2], &s1);
        if (out_path) { out = fopen(out_path, "w"); if (!out) { perror("fopen"); return 1; } }
        print_single(out, argv[2], &s1);
        if (to_term && out != stdout) { print_single(stdout, argv[2], &s1); }
    } else if (!strcmp(mode, "diff")) {
        if (argc < 4) { fprintf(stderr, "diff 模式需要两个日志文件\n"); return 1; }
        analyze_file(argv[2], &s1);
        analyze_file(argv[3], &s2);
        if (out_path) { out = fopen(out_path, "w"); if (!out) { perror("fopen"); return 1; } }
        print_diff(out, argv[2], argv[3], &s1, &s2);
        if (to_term && out != stdout) { print_diff(stdout, argv[2], argv[3], &s1, &s2); }
    } else {
        fprintf(stderr, "未知模式: %s（single / diff）\n", mode);
        return 1;
    }

    if (out_path && out != stdout) { fclose(out); fprintf(stderr, "[info] 报告已写入 %s\n", out_path); }
    return 0;
}
