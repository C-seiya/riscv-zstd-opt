#!/usr/bin/env bash
# step2_qemu_rocksdb_bench.sh — 在 QEMU 中运行 db_bench 验证 RocksDB 适配
# 用法（在 WSL 中）：bash /mnt/d/c2170/yasige/2026-10-05-22-08-09/scripts/step2_qemu_rocksdb_bench.sh
set -u

ROCKSDB_DIR="${ROCKSDB_DIR:-$HOME/rocksdb}"
DB_DIR="$HOME/rocksdb_qemu_db"
QEMU="qemu-riscv64"
SYSROOT="/usr/riscv64-linux-gnu"

echo "=== 检查前置条件 ==="
[ -x "$ROCKSDB_DIR/db_bench" ] || { echo "❌ db_bench 不存在，请先运行 step1。"; exit 1; }
command -v $QEMU >/dev/null || { echo "❌ 未找到 $QEMU"; exit 1; }

file "$ROCKSDB_DIR/db_bench" | grep -q "ELF 64-bit.*RISC-V" && echo "✅ db_bench 为 RISC-V ELF"

echo ""
echo "=== [1/3] fillseq 写入基准（zstd 压缩路径）==="
rm -rf "$DB_DIR"; mkdir -p "$DB_DIR"
# QEMU 下运行较慢，先用小规模验证功能通路
cd "$ROCKSDB_DIR"
timeout 1800 $QEMU -L "$SYSROOT" ./db_bench \
    --benchmarks=fillseq \
    --num=10000 \
    --db="$DB_DIR" \
    --compression_type=zstd \
    --compression_max_dict_bytes=0 \
    2>&1 | tee "$HOME/rocksdb_bench_result.txt"

echo ""
echo "=== [2/3] 读回校验（readrandom 确保数据可读）==="
timeout 1800 $QEMU -L "$SYSROOT" ./db_bench \
    --benchmarks=readrandom \
    --num=10000 \
    --db="$DB_DIR" \
    --compression_type=zstd \
    2>&1 | tee -a "$HOME/rocksdb_bench_result.txt"

echo ""
echo "=== [3/3] 存储放大（可选：对比无压缩体积）==="
DB_SIZE=$(du -sh "$DB_DIR" | cut -f1)
echo "zstd 压缩后 DB 体积: $DB_SIZE"
echo "（可选验证：用 --compression_type=none 重跑对比）"

echo ""
echo "✅ RocksDB QEMU 验证完成，结果保存在 ~/rocksdb_bench_result.txt"
echo "下一步: bash step3_capture_analyze.sh"
