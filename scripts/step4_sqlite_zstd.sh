#!/usr/bin/env bash
# step4_sqlite_zstd.sh — 第二框架（SQLite + Zstd）构建与 QEMU 验证
# 用法（在 WSL 中）：bash /mnt/d/c2170/yasige/2026-10-05-22-08-09/scripts/step4_sqlite_zstd.sh
set -u

PROJ_SRC="/mnt/d/c2170/yasige/2026-10-05-22-08-09"
WORK="$HOME/sqlite_zstd"
QEMU="qemu-riscv64"
SYSROOT="/usr/riscv64-linux-gnu"
CROSS="riscv64-linux-gnu-"

mkdir -p "$WORK"; cd "$WORK"

echo "=== [1/5] 获取 SQLite amalgamation ==="
if [ ! -f sqlite3.c ]; then
    # 优先本地源码，其次国内镜像，最后官网
    if [ -f /usr/include/sqlite3.h ] && command -v apt >/dev/null; then
        apt-get source sqlite3 2>/dev/null || true
    fi
    if [ ! -f sqlite3.c ]; then
        YEAR=2025
        # 尝试多个已知 amalgamation 版本
        for url in \
            "https://mirrors.tuna.tsinghua.edu.cn/" \
            "https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip" \
            "https://www.sqlite.org/2024/sqlite-amalgamation-3450100.zip"; do
            echo "尝试下载: $url"
            if wget -q --timeout=30 "$url" -O sqlite-amalgamation.zip 2>/dev/null; then
                break
            fi
        done
        [ -f sqlite-amalgamation.zip ] && unzip -o sqlite-amalgamation.zip && \
            cp sqlite-amalgamation-*/sqlite3.c sqlite3.c 2>/dev/null
    fi
fi

if [ ! -f sqlite3.c ]; then
    echo "⚠️ 自动下载失败。请手动获取 sqlite-amalgamation（含 sqlite3.c）放入 $WORK/ 后重跑。"
    echo "   官网: https://www.sqlite.org/download.html  (amalgamation zip)"
    exit 1
fi
echo "✅ sqlite3.c 就绪"

echo ""
echo "=== [2/5] 交叉编译 SQLite（riscv64 静态库）==="
${CROSS}gcc -O2 -c sqlite3.c -o sqlite3.o -DSQLITE_THREADSAFE=0 \
    -DHAVE_USLEEP=1 -DSQLITE_ENABLE_ZIPV=0
ar rcs libsqlite3.a sqlite3.o
echo "✅ libsqlite3.a 就绪"

echo ""
echo "=== [3/5] 编译 sqlite_zstd_bench（链接优化后的 zstd）==="
cp "$PROJ_SRC/app/sqlite_zstd_bench.c" .
ZSTD_LIB="${ZSTD_LIB:-$HOME/zstd_backup}"   # 优化版 zstd 所在目录（含 lib/ 与 include/）
ZSTD_INC="-I${ZSTD_LIB}/include -I/usr/include"
ZSTD_LD="-L${ZSTD_LIB}/lib -L/usr/riscv64-linux-gnu/lib"

${CROSS}gcc -O2 -static sqlite_zstd_bench.c sqlite3.o \
    $ZSTD_INC $ZSTD_LD -lzstd -lpthread -ldl -lm \
    -o sqlite_zstd_bench \
    && echo "✅ sqlite_zstd_bench 编译成功" \
    || { echo "❌ 编译失败：请检查 ZSTD_LIB 路径（优化版 zstd 库目录）"; exit 1; }
file sqlite_zstd_bench | grep -q "RISC-V" && echo "✅ RISC-V ELF 确认"

echo ""
echo "=== [4/5] QEMU 运行基准 ==="
rm -f "$WORK/bench.db"
timeout 1800 $QEMU -L "$SYSROOT" ./sqlite_zstd_bench \
    --db "$WORK/bench.db" --rows 500 --blob-size 4096 \
    2>&1 | tee "$WORK/sqlite_bench_result.txt"

echo ""
echo "=== [5/5] 抓取指令日志并用 analysis_v2 分析 ==="
rm -f "$WORK/bench.db"
timeout 3600 $QEMU -L "$SYSROOT" -d in_asm \
    ./sqlite_zstd_bench --db "$WORK/bench.db" --rows 100 --blob-size 4096 \
    &> "$WORK/sqlite_inst.log"

if [ -x "$HOME/analysis_v2" ]; then
    "$HOME/analysis_v2" single "$WORK/sqlite_inst.log" -o "$WORK/sqlite_mix.md"
    echo "✅ SQLite 指令混合度: $WORK/sqlite_mix.md"
fi

echo ""
echo "✅ 第二框架（SQLite + Zstd）适配验证完成！"
echo "产出："
echo "   - $WORK/sqlite_bench_result.txt （吞吐/压缩比/完整性校验结果）"
echo "   - $WORK/sqlite_mix.md           （指令混合度）"
