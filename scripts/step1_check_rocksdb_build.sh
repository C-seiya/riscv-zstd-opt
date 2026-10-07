#!/usr/bin/env bash
# step1_check_rocksdb_build.sh — 检查/续编 RocksDB db_bench
# 用法（在 WSL 中）：bash /mnt/d/c2170/yasige/2026-10-05-22-08-09/scripts/step1_check_rocksdb_build.sh
set -u

ROCKSDB_DIR="${ROCKSDB_DIR:-$HOME/rocksdb}"
BUILD_LOG="${BUILD_LOG:-$HOME/rocksdb_build.log}"

echo "=== [1/4] 检查旧的编译进程 ==="
# 检查是否还有 make 进程在跑（之前 PID 1497 状态 D）
if pgrep -f "make.*rocksdb|make db_bench" >/dev/null 2>&1; then
    echo "make 进程仍在运行："
    pgrep -af "make" | head -5
    echo "等待其完成（每 30s 轮询一次，最多 20 分钟）..."
    for i in $(seq 1 40); do
        sleep 30
        if ! pgrep -f "make.*rocksdb|make db_bench" >/dev/null 2>&1; then
            echo "编译进程已结束。"
            break
        fi
        echo "  ...已等待 $((i*30))s"
    done
else
    echo "无 make 进程在运行（原 PID 1497 已退出）。"
fi

echo ""
echo "=== [2/4] 检查 db_bench 是否已生成 ==="
if [ -x "$ROCKSDB_DIR/db_bench" ]; then
    ls -lh "$ROCKSDB_DIR/db_bench"
    echo "✅ db_bench 已生成。"
    exit 0
fi
echo "❌ db_bench 尚未生成，检查构建日志尾部："
[ -f "$BUILD_LOG" ] && tail -n 20 "$BUILD_LOG" || echo "(无构建日志 $BUILD_LOG)"

echo ""
echo "=== [3/4] 检查 librocksdb.a 静态库 ==="
if [ -f "$ROCKSDB_DIR/librocksdb.a" ]; then
    ls -lh "$ROCKSDB_DIR/librocksdb.a"
    echo "✅ 静态库已就绪，只需补编 db_bench。"
else
    echo "⚠️ 静态库不存在，可能需要完整重新构建。"
fi

echo ""
echo "=== [4/4] 交叉编译 db_bench（release + 静态库）==="
cd "$ROCKSDB_DIR" || exit 1

# ---- 关键构建参数说明（踩坑记录）----
# 1. CC/CXX 必须显式指定交叉工具链，否则裸 make 用宿主 x86 gcc；
#    与之前构建参数不一致还会触发 "Build parameters changed ... stale" 报错
# 2. DEBUG_LEVEL=0：默认是 1（debug 模式），对象文件巨大、编译极慢
#    （之前 PID 1497 长时间 D 状态就是这个原因）
# 3. LIB_MODE=static：生成 librocksdb.a（STATIC_BUILDING 是无效变量，已移除）
# 4. AUTO_CLEAN=1：自动清理参数变化产生的陈旧对象，避免手动 make clean
# 5. EXTRA_LDFLAGS=-static：db_bench 静态链接，QEMU 用户态免依赖 sysroot 动态库

CROSS="${CROSS:-riscv64-linux-gnu-}"
JOBS="${JOBS:-8}"   # 默认 8 并行，避免 -j32 造成磁盘 I/O 打满（D 状态）
export PATH="$HOME/rocksdb-env/bin:$PATH"   # RuyiSDK 虚拟环境（若存在则优先生效）

if ! command -v "${CROSS}gcc" >/dev/null 2>&1; then
    echo "❌ 未找到 ${CROSS}gcc，请确认交叉工具链已安装或已激活 RuyiSDK 环境"
    exit 1
fi

MAKE_ARGS=(
    -j"$JOBS" db_bench
    CC="${CROSS}gcc"
    CXX="${CROSS}g++"
    AR="${CROSS}ar"
    LD="${CROSS}ld"
    DEBUG_LEVEL=0
    LIB_MODE=static
    AUTO_CLEAN=1
)

echo "第一次尝试：静态链接"
echo "  make ${MAKE_ARGS[*]} EXTRA_LDFLAGS=-static"
if ! make "${MAKE_ARGS[@]}" EXTRA_LDFLAGS="-static" 2>&1 | tee -a "$BUILD_LOG"; then
    echo ""
    echo "⚠️ 静态链接失败（通常是缺 riscv64 版 .a 依赖如 libgflags），回退动态链接重试..."
    echo "  make ${MAKE_ARGS[*]}"
    if ! make "${MAKE_ARGS[@]}" 2>&1 | tee -a "$BUILD_LOG"; then
        echo "❌ 编译失败，请查看 $BUILD_LOG 排错。"
        exit 1
    fi
fi

if [ -x "$ROCKSDB_DIR/db_bench" ]; then
    echo ""
    echo "✅✅✅ db_bench 编译成功！"
    ls -lh "$ROCKSDB_DIR/db_bench"
    file "$ROCKSDB_DIR/db_bench" | grep -q "RISC-V" \
        && echo "✅ RISC-V ELF 确认" \
        || echo "⚠️ 警告：db_bench 似乎不是 RISC-V ELF，请检查工具链设置！"
    [ -f "$ROCKSDB_DIR/librocksdb.a" ] && ls -lh "$ROCKSDB_DIR/librocksdb.a"
    echo ""
    echo "下一步: bash step2_qemu_rocksdb_bench.sh"
else
    echo ""
    echo "❌ 编译仍未产出 db_bench，请查看 $BUILD_LOG 排错。"
    echo "常见排查："
    echo "  1) 缺 gflags: sudo apt install libgflags-dev 的 riscv64 交叉版"
    echo "  2) 依赖 .so 找不到: 在 make 参数追加 EXTRA_LDFLAGS=\"-L/usr/riscv64-linux-gnu/lib\""
    echo "  3) 强制全量重建: 先 make clean 再重跑本脚本"
    exit 1
fi
