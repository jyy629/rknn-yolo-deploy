#!/bin/bash
set -e

# ======================== 配置 ========================
GXX="/home/jyy/rk3568/rk3568_arm_gcc/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-g++"
SYSROOT="/home/jyy/rk3568/rk3568_arm_gcc/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/aarch64-none-linux-gnu/libc"
PROJECT_ROOT="/home/jyy/rk3568/project"

# 源文件所在目录（即本脚本所在目录）
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/run.cc"
HTTP_SRC="$PROJECT_ROOT/any_source/http_0.0.0.cc"
YOLO_SRC="$PROJECT_ROOT/any_source/yolov8_0.0.0.cc"

# ======================== 参数检查 ========================
if [ $# -lt 1 ]; then
    echo "用法: $0 <模型路径>"
    echo "示例: $0 /home/jyy/rk3568/project/models/v8/yolov8s_normal_800448_plan_A.rknn"
    exit 1
fi

MODEL_SRC="$1"
if [ ! -f "$MODEL_SRC" ]; then
    echo "错误: 模型文件不存在 → $MODEL_SRC"
    exit 1
fi

MODEL_NAME=$(basename "$MODEL_SRC")

# ======================== 清理并创建 running ========================
RUNNING_DIR="$SCRIPT_DIR/running"
rm -rf "$RUNNING_DIR"
mkdir -p "$RUNNING_DIR/lib"

# ======================== 编译 ========================
echo "===== 编译 set_cam_1.1 ====="
$GXX "$SRC" "$HTTP_SRC" "$YOLO_SRC" -o "$RUNNING_DIR/set_cam_1.1" \
  -std=c++17 -O0 -g -mcpu=cortex-a55 \
  -I"$SYSROOT/usr/include/gstreamer-1.0" \
  -I"$SYSROOT/usr/include/glib-2.0" \
  -I"$SYSROOT/usr/lib/gstreamer/glib-2.0/include" \
  -I"$SYSROOT/usr/include/mpp" \
  -I"$SYSROOT/usr/include" \
  -I"$SYSROOT/usr/include/opencv4" \
  -I"$PROJECT_ROOT/any_include" \
  -L"$SYSROOT/usr/lib/gstreamer" \
  -L"$SYSROOT/usr/lib/mpp" \
  -L"$SYSROOT/usr/lib/aarch64-linux-gnu" \
  -L"$SYSROOT/usr/lib/opencv" \
  -L"$SYSROOT/usr/lib/rga" \
  -L"$SYSROOT/usr/lib/rknn" \
  -l:libgstreamer-1.0.so \
  -l:libgstapp-1.0.so \
  -l:libgstbase-1.0.so \
  -l:libglib-2.0.so \
  -l:libgobject-2.0.so \
  -l:libgio-2.0.so \
  -l:librockchip_mpp.so \
  -l:libopencv_core.so.414 \
  -l:libopencv_imgproc.so.414 \
  -l:libopencv_imgcodecs.so.414 \
  -lrga \
  -lrknnrt \
  -lpthread -lm \
  -Wl,--allow-shlib-undefined \
  -Wl,-rpath='$ORIGIN/lib'

echo "===== 编译成功 ====="
file "$RUNNING_DIR/set_cam_1.1"
echo ""

# ======================== 拷贝 run_devices.sh ========================
echo "===== 拷贝 run_devices.sh ====="
cp "$SCRIPT_DIR/run_devices.sh" "$RUNNING_DIR/run_devices.sh"
chmod +x "$RUNNING_DIR/run_devices.sh"
echo "  run_devices.sh → running/"

echo "===== 拷贝 use_check.sh ====="
cp "$SCRIPT_DIR/use_check.sh" "$RUNNING_DIR/use_check.sh"
chmod +x "$RUNNING_DIR/use_check.sh"
echo "  use_check.sh → running/"

# ======================== 拷贝模型 ========================
echo "===== 拷贝模型 ====="
cp "$MODEL_SRC" "$RUNNING_DIR/$MODEL_NAME"
echo "  $MODEL_NAME"

# ======================== 生成 set.cfg（model_path 指向运行目录下的模型） ========================
echo "===== 准备 set.cfg ====="
if [ -f "$SCRIPT_DIR/set.cfg" ]; then
    sed "s|^model_path=.*|model_path=./$MODEL_NAME|" "$SCRIPT_DIR/set.cfg" > "$RUNNING_DIR/set.cfg"
fi
echo "  set.cfg → model_path=./$MODEL_NAME"

# ======================== 拷贝 .so 依赖库 ========================
echo "===== 拷贝依赖库 ====="

LIBS=(
    "$SYSROOT/usr/lib/gstreamer/libgstreamer-1.0.so"
    "$SYSROOT/usr/lib/gstreamer/libgstapp-1.0.so"
    "$SYSROOT/usr/lib/gstreamer/libgstbase-1.0.so"
    "$SYSROOT/usr/lib/gstreamer/libglib-2.0.so"
    "$SYSROOT/usr/lib/gstreamer/libgobject-2.0.so"
    "$SYSROOT/usr/lib/gstreamer/libgio-2.0.so"
    "$SYSROOT/usr/lib/mpp/librockchip_mpp.so"
    "$SYSROOT/usr/lib/opencv/libopencv_core.so.414"
    "$SYSROOT/usr/lib/opencv/libopencv_imgproc.so.414"
    "$SYSROOT/usr/lib/opencv/libopencv_imgcodecs.so.414"
    "$SYSROOT/usr/lib/rga/librga.so"
    "$SYSROOT/usr/lib/rknn/librknnrt.so"
)

for lib in "${LIBS[@]}"; do
    if [ -f "$lib" ]; then
        # -L 追踪符号链接：opencv .so.414 → .so.4.14.0 会被解析为真实文件
        # 但目标文件名仍用符号链接的名字（如 libopencv_core.so.414）
        cp -L "$lib" "$RUNNING_DIR/lib/"
        echo "  $(basename "$lib")"
    else
        echo "  [警告] 缺失: $lib"
    fi
done

# ======================== 完成 ========================
echo ""
echo "===== running 目录 ======"
find "$RUNNING_DIR" -type f | sed "s|$RUNNING_DIR/|  |" | sort
echo ""
echo "===== 构建完成 ====="
echo "  running/set_cam_1.1    ← 可执行程序 (rpath=\$ORIGIN/lib)"
echo "  running/lib/           ← 依赖 .so 库"
echo "  running/set.cfg        ← 配置文件"
echo "  running/run_devices.sh  ← 板端启动脚本"
echo "  running/$MODEL_NAME    ← 模型文件"
echo ""
echo "下一步:"
echo "  板端 → cd running && ./run_devices.sh"
echo "  主机 → ./run_pc.sh"