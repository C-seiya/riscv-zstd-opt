#!/usr/bin/env bash
# run_all.sh — 一键串联：RocksDB 收尾 → QEMU 验证 → 指令分析 → SQLite 第二框架
# 用法（在 WSL 中）：bash /mnt/d/c2170/yasige/2026-10-05-22-08-09/scripts/run_all.sh
set -u
S=/mnt/d/c2170/yasige/2026-10-05-22-08-09/scripts

echo "╔══════════════════════════════════════════════╗"
echo "║  RISC-V Zstd 优化项目 — 全流程执行           ║"
echo "╚══════════════════════════════════════════════╝"

echo ""
echo ">>> 步骤 1/4: 检查/续编 RocksDB db_bench"
bash "$S/step1_check_rocksdb_build.sh" || { echo "步骤1失败，中止。"; exit 1; }

echo ""
echo ">>> 步骤 2/4: QEMU 中运行 db_bench"
bash "$S/step2_qemu_rocksdb_bench.sh" || { echo "步骤2失败，中止。"; exit 1; }

echo ""
echo ">>> 步骤 3/4: 抓取 RocksDB 指令日志并分析"
bash "$S/step3_capture_analyze.sh" || { echo "步骤3失败，中止。"; exit 1; }

echo ""
echo ">>> 步骤 4/4: SQLite + Zstd 第二框架"
bash "$S/step4_sqlite_zstd.sh" || { echo "步骤4失败。"; }

echo ""
echo "╔══════════════════════════════════════════════╗"
echo "║  全流程完成！产出汇总：                       ║"
echo "║  RocksDB:                                     ║"
echo "║    ~/rocksdb_bench_result.txt  基准结果       ║"
echo "║    ~/rocksdb_inst.log          指令轨迹       ║"
echo "║    ~/rocksdb_mix.md            混合度统计     ║"
echo "║  SQLite:                                      ║"
echo "║    ~/sqlite_zstd/sqlite_bench_result.txt      ║"
echo "║    ~/sqlite_zstd/sqlite_mix.md                ║"
echo "║  将这些数据填入 docs/02、docs/03 报告即可。    ║"
echo "╚══════════════════════════════════════════════╝"
