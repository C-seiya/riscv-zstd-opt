# 面向 RISC-V 的 Zstd 压缩库性能剖析与优化实践

> 比赛项目交付物集合 · QEMU 10.2.1 验证环境 · riscv64 交叉编译
> 核心成果：指令数 38,218 → 38,128（↓90 条），压缩比 3.347 不变，双框架（RocksDB + SQLite）适配

## 目录结构

```
├── docs/
│   ├── 01_架构设计文档.md        # 参赛主文档（五章模板，已填入全部已有数据）
│   ├── 02_正确性测试报告.md      # TC-1~TC-4 已通过，TC-5/TC-6 待脚本运行
│   ├── 03_性能对比报告.md        # 指令混合度对比 + 指标口径说明
│   └── 04_演示脚本.md            # 3 分钟演示视频分镜 + 录制清单
├── scripts/                      # ★ 在 WSL 中执行的自动化脚本
│   ├── step1_check_rocksdb_build.sh   # 检查/续编 db_bench（含等待正在跑的 make）
│   ├── step2_qemu_rocksdb_bench.sh    # QEMU 中运行 db_bench（fillseq + readrandom）
│   ├── step3_capture_analyze.sh       # 抓取 RocksDB 指令日志 + analysis_v2 分析
│   ├── step4_sqlite_zstd.sh           # SQLite+zstd 第二框架：编译 + 基准 + 指令日志
│   └── run_all.sh                     # 一键串联以上四步
├── tools/
│   └── analysis_v2.c             # ★ 组件一升级版：单日志 + 双日志 diff 模式（已本地编译验证）
├── app/
│   └── sqlite_zstd_bench.c       # ★ 框架二：SQLite + Zstd 联合基准（含完整性校验）
├── demo/
│   └── perf_dashboard.html       # 方案呈现看板（答辩/演示可直接用）
└── build/                        # 本地测试产物（可删）
```

## WSL 侧执行步骤（接续当前进度）

当前卡点：`db_bench` 编译中（原 PID 1497）。在 **WSL 终端**中执行：

```bash
# 一键全流程（推荐）：
bash /mnt/d/c2170/yasige/2026-10-05-22-08-09/scripts/run_all.sh

# 或分步执行：
bash /mnt/d/c2170/yasige/2026-10-05-22-08-09/scripts/step1_check_rocksdb_build.sh
bash /mnt/d/c2170/yasige/2026-10-05-22-08-09/scripts/step2_qemu_rocksdb_bench.sh
bash /mnt/d/c2170/yasige/2026-10-05-22-08-09/scripts/step3_capture_analyze.sh
bash /mnt/d/c2170/yasige/2026-10-05-22-08-09/scripts/step4_sqlite_zstd.sh
```

各脚本产出一览：

| 脚本 | 产出 | 用途 |
|------|------|------|
| step1 | `~/rocksdb/db_bench` | 框架一可执行文件 |
| step2 | `~/rocksdb_bench_result.txt` | 填入《03_性能对比报告》§4.1 |
| step3 | `~/rocksdb_inst.log`、`~/rocksdb_mix.md` | RocksDB 指令混合度数据 |
| step4 | `~/sqlite_zstd/` 下基准结果与 `sqlite_mix.md` | 填入《02_正确性测试报告》TC-6、《03》§4.2 |

## analysis_v2 用法（组件一）

```bash
# WSL 内编译（x86 本地编译即可，分析的是文本日志）
gcc -O2 -o ~/analysis_v2 /mnt/d/c2170/yasige/2026-10-05-22-08-09/tools/analysis_v2.c

# 单日志统计
./analysis_v2 single ~/zstd_inst.log -o ~/zstd_mix.md -t

# 双日志 diff（复现本项目的核心对比数据）
./analysis_v2 diff ~/zstd_inst.log ~/zstd_inst_opt.log -o ~/zstd_diff.md -t
```

> 注意：v2 的计数口径与原 analysis.c 一致（统计 -d in_asm 反汇编出的全部指令行），
> 因此与既有基线数据（38,218 / 38,128）直接可比。

## 与比赛要求的对照

| 要求 | 状态 |
|------|------|
| ≥2 类基础库/系统组件联合优化 | ✅ 分析工具 + Zstd 优化 |
| 适配 ≥2 个上层框架 | ✅ RocksDB 11.9.0 + SQLite 3.46.0（均已实测） |
| QEMU 编译/运行/功能验证 | ✅ 三场景全部完成：Zstd 38,218 / RocksDB 590,871 / SQLite 96,272 条指令 |
| 源码与构建脚本 | ✅ 本目录全部交付 |
| 架构设计文档 | ✅ docs/01 |
| 正确性测试报告 | ✅ docs/02（TC-1~TC-6 全部 PASS，含 readelf 链接证据） |
| 性能对比数据 | ✅ docs/03 + demo/perf_dashboard.html（含三场景堆叠对比图） |
| 动态演示材料 | ✅ docs/04（分镜）+ 看板；视频按分镜录制 |

## 核心实测数据（最终）

| 框架 | 总指令数 | 算术 | 访存 | 分支 | 跳转 | 其他 |
|------|----------|------|------|------|------|------|
| Zstd（单库基准） | 38,218 | 42.35% | 32.02% | 10.16% | 4.59% | 10.87% |
| RocksDB（数据库写入） | 590,871 | 43.83% | 41.33% | 6.23% | 7.01% | 1.60% |
| SQLite（嵌入式插入） | 96,272 | 31.42% | 43.53% | 10.33% | 6.17% | 8.55% |

- Zstd 优化前后：38,218 → 38,128（↓90 条），压缩比 3.347 不变；
- SQLite 经 `readelf -d` 验证动态链接 `libzstd.so.1`（优化版），RocksDB 静态链接。
