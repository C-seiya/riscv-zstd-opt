#!/usr/bin/env bash
# step3_capture_analyze.sh — 抓取 RocksDB 指令日志并用 analysis_v2 分析
# 用法（在 WSL 中）：bash /mnt/d/c2170/yasige/2026-10-05-22-08-09/scripts/step3_capture_analyze.sh
set -u

ROCKSDB_DIR="${ROCKSDB_DIR:-$HOME/rocksdb}"
DB_DIR="$HOME/rocksdb_qemu_db"
QEMU="qemu-riscv64"
SYSROOT="/usr/riscv64-linux-gnu"
LOG="$HOME/rocksdb_inst.log"
PROJ_SRC="/mnt/d/c2170/yasige/2026-10-05-22-08-09"

echo "=== [1/3] 抓取 db_bench 指令轨迹（fillseq, 1000 条，控制日志体积）==="
rm -rf "$DB_DIR"; mkdir -p "$DB_DIR"
cd "$ROCKSDB_DIR"
# 提示：-d in_asm 的日志会非常大，这里把 num 减小到 1000 控制体积
timeout 3600 $QEMU -L "$SYSROOT" -d in_asm \
    ./db_bench --benchmarks=fillseq --num=1000 --db="$DB_DIR" --compression_type=zstd \
    &> "$LOG"

LOG_SIZE=$(du -h "$LOG" | cut -f1)
echo "✅ 指令日志已生成: $LOG ($LOG_SIZE)"

echo ""
echo "=== [2/3] 准备 analysis_v2 工具 ==="
if [ ! -x "$HOME/analysis_v2" ]; then
    if [ -f "$PROJ_SRC/tools/analysis_v2.c" ]; then
        cp "$PROJ_SRC/tools/analysis_v2.c" "$HOME/"
    fi
    # WSL 内用本地 gcc 编译分析工具（x86 即可，分析的是文本日志）
    gcc -O2 -o "$HOME/analysis_v2" "$HOME/analysis_v2.c" -lm \
        && echo "✅ analysis_v2 编译完成" \
        || { echo "❌ analysis_v2 编译失败"; exit 1; }
else
    echo "✅ analysis_v2 已存在"
fi

echo ""
echo "=== [3/3] 单日志统计 RocksDB 指令混合度 ==="
"$HOME/analysis_v2" single "$LOG" -o "$HOME/rocksdb_mix.md"

echo ""
echo "✅ 分析完成。产出："
echo "   - $HOME/rocksdb_mix.md （RocksDB 指令混合度统计）"
echo ""
echo "如需与『未优化 zstd』版本做 diff 对比："
echo "   1) 用系统 zstd-dev 的 librocksdb 重跑一次抓 log_base"
echo "   2) ./analysis_v2 diff log_base $LOG -o rocksdb_diff.md"
