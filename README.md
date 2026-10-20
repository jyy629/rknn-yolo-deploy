# RKNN 端侧实时目标检测部署 Demo（YOLOv8 / YOLOv5）

面向 **Rockchip RK3568** 的端侧实时目标检测部署工程。**当前已完成单路端到端验证，多路流水线架构已实现但尚未做压测。** 跑通
**RTSP 拉流 → 硬件解码 → RGA 缩放/转色 → RKNN NPU 推理 → 后处理 → HTTP 推流** 全链路，
从硬解、连续内存、环形缓存、SIMD 后处理到量化误差分析一整套都自己实现，
并附带模型裁剪工具与逐层量化精度/性能分析。

> 开发时间：2026.03 – 2026.06　·　平台：Rockchip RK3568
> C++17（板端推理管线）· Bash（交叉编译 / 部署）· Python（ONNX 裁剪工具）

---

## 技术要点

**全硬件流水线，全程不落 CPU 拷贝。**
RTSP 拉流 → **MPP 硬件解码（H.265 / H.264）** → **RGA 缩放与色彩转换（NV12→RGB）** → **RKNN NPU 推理**，
帧数据走 **DMA-Heap(CMA) 物理连续内存** 并通过 `rknn_set_io_mem` 直接绑定为模型 I/O 张量，
避免中间帧在 CPU 与 NPU 之间来回搬运，把延迟压在内存带宽这一环之下。

**有界环形缓存 + 丢帧反压，端到端不堆积。**
自研模板化环形队列 `circular_list<T, N>`，流水线每一级之间用独立环形缓存解耦；
队列写满时**自动丢弃最旧帧并触发回调释放硬解帧**（`mpp_frame_deinit`），
消费者侧用条件变量唤醒、无帧时非阻塞让出 CPU——高负载下主动丢帧保证延迟稳定、内存不涨。

**ARM NEON 向量化热点后处理。**
后处理最吃 CPU 的两处都用 NEON 重写：
16 路 int8 并行做置信度阈值筛选（`vcgeq_s8` 出位掩码 → `__builtin_ctzll` 只枚举命中项，跳过绝大多数无效格点）；
NMS 的 IoU 用 `float32x4` 四路并行计算，配合位图（bitmask）批量置位剔除。

**反量化查表（LUT），后处理零浮点乘除。**
预构建 256 项**反量化表 / sigmoid 表 / exp 表**，运行时把 `(int8 - zp) * scale`、`sigmoid`、`exp` 全部换成一次查表，
把后处理里的浮点乘法链直接抹掉。

**多线程多输出头并行解码。**
YOLOv5 的三个输出头各起一个 worker 线程，用条件变量做令牌式派发（`dispatch_and_wait`）与主线程同步，
三个头的解码真正并行，随后统一 NMS。

**量化精度可诊断、可定位。**
内置逐层**量化精度分析**（simulator/runtime 的 cos 相似度与欧氏误差）与逐层**性能分析**（Cycles / MacUsage / 耗时），
并支持导出逐类命中统计、反量化直方图等诊断报告，用来定位是「哪一层把精度搞崩的」。

**原生中文标签渲染。**
自带点阵中文字库，直接往帧缓冲（RGB）上绘制中文类别名，不依赖 freetype 等任何重依赖。

---

## 性能实测（RK3568，单路，热稳定）

> 平台 Rockchip RK3568 · 模型 YOLOv8s · 输入 800×448 · 置信度阈值 int8≥32 · IoU 0.5

| 阶段 | Plane A | Plane B |
|---|---|---|
| **NPU 推理** | ~80 ms | ~100 ms |
| sort + box 搜索 | 0.6 ms | 0.4 ms |
| NMS | 0.08 ms | 0.06 ms |
| **总耗时** | **~81 ms** | **~101 ms** |
| **帧率** | **~12.3 FPS** | **~9.9 FPS** |

- **Plane A 比 Plane B 快约 20%**：因为裁掉了 NPU 上的 DFL 卷积层（首次推理有 DVFS 冷启动 penalty +50~70ms，热跑升频后稳定）。
- **NPU 是唯一瓶颈**：单帧 ~80ms 占端到端约 **99%**，CPU 侧后处理（搜索 + NMS）合计 **< 1ms** 可忽略——优化重心应继续压在 NPU 侧。
- **后处理开销极小**：阈值 32 下 7350 个 anchor 仅命中 82 个（**1.12%**），NMS 后 **82 → 10 框**；NEON 筛选 + LUT 反量化把后处理压到亚毫秒级。
- **DFL 数值已验证**：box logits 实测范围 `[-4.60, 17.39]`，`exp` 不溢出 float32，故 DFL softmax 直接查 `exp_table[256]`、无需减最大值。

---

## 已知限制（Known Limitations）

- **多路未压测**：当前仅完成 **单路** RTSP 端到端验证；多路（1–5 路）流水线架构与调度已实现，但 **尚未进行多路并发压力测试**。
- **单路瓶颈**：单路 800×448 下 NPU 推理占端到端约 99% 耗时，为主要瓶颈；多路场景预计需降低分辨率或跳帧。
- **模型精度**：裁剪 / 量化后经数值一致性校验（误差 < 1e-3），尚未在标准数据集上进行 mAP 评测。

---

## 流水线架构

```
   RTSP 摄像头 (1..N 路)
        │
        ▼
 ┌────────────┐   ┌──────────────┐   ┌──────────────┐   ┌──────────────┐   ┌────────────┐
 │  PSQ_gts   │ → │  PSQ_vpu     │ → │  PSQ_rga     │ → │  PSQ_rknn /  │ → │  PSQ_http  │
 │ GStreamer  │   │ MPP 硬解码   │   │ 缩放 + 转色  │   │  YOLOv8/5    │   │ MJPEG 推流 │
 │ rtspsrc    │   │ H265 / H264  │   │ NV12→RGB     │   │ NPU + 后处理 │   │ :8080+     │
 └────────────┘   └──────────────┘   └──────────────┘   └──────────────┘   └────────────┘
        └──── 每级之间 = 独立线程 + 有界环形缓存（circular_list）────┘
                                              PSQ_observer（USE 设备利用率监控）
```

每一级都是「独立线程 + 环形缓存」的生产者—消费者，摄像头路数在运行时配置，各级解耦、可独立观测。

---

## 目录结构

```
demo/
├── any_include/            # 通用模块头文件（按职责拆分）
│   ├── gts_0.0.0.hh        # GStreamer RTSP 拉流
│   ├── vpu_0.0.0.hh        # MPP 硬件解码 (H265/H264)
│   ├── vpu_0.0.1.hh
│   ├── rga_0.0.0.hh        # RGA 缩放 / 格式转换
│   ├── rga_v4l2_0.0.0.hh
│   ├── v4l2_0.0.0.hh       # V4L2 本地摄像头采集
│   ├── rkn_0.0.0.hh        # RKNN 上下文 / 张量 / 内存封装
│   ├── yolov8_0.0.0.hh     # YOLOv8 后处理
│   ├── yolov5_0.0.0.hh     # YOLOv5 后处理
│   ├── http_0.0.0.hh       # HTTP MJPEG 服务
│   ├── observer.hh         # USE 利用率观测
│   ├── strategy.hh         # 管线调度 + 环形缓存（核心）
│   ├── creat_cma.h         # DMA-Heap 连续内存分配
│   └── sync_0.0.0.h
├── any_source/             # 上述模块的实现
│   ├── yolov8_0.0.0.cc     # 后处理（NEON 筛选 / LUT 反量化 / DFL / NMS）
│   ├── yolov5_0.0.0.cc     # 后处理（多线程多头部 / NEON NMS / 中文标签）
│   └── http_0.0.0.cc       # MJPEG 推流
├── yolo8_1.1.0/            # ★ 主程序：多路 RTSP 管线
│   ├── run.cc              # 入口：解析 set.cfg → 组装并启动管线
│   ├── set.cfg             # 运行配置
│   ├── build.sh            # 交叉编译 + 打包 running/
│   ├── run_devices.sh      # 板端：自动配网 → 校验 → 启动
│   ├── run_pc.sh           # PC 端：adb 推送 → 配网 → 端口转发 → 启动
│   ├── use_check.sh        # USE 监控面板
│   └── running/            # （构建产物，已 gitignore）
├── yolov5.1.0.3/           # YOLOv5 单路 demo（V4L2 采集）
│   ├── yolo.cc             # 入口
│   ├── include/  scrip/    # 头文件与实现
│   └── build/  system_so/  yolo_so/   # （构建产物，已 gitignore）
├── fix_a_auto.py           # ONNX 深度裁剪工具（YOLOv8 plan A）
├── plane_A.txt / plane_B.txt  # RKNN 量化精度分析（逐层 cos/euc 误差）
├── evel_planA              # RKNN 性能/内存分析（逐层 Cycles / MacUsage / 耗时）
└── *.rknn                  # 预转换的 RKNN 模型（v5s / v8s plan A / B）
```

---

## 关键技术实现

### 环形缓存 `circular_list<T, N>`（`strategy.hh`）

- 固定 N 槽的有界队列（拉流/解码 3 槽，RGA/推理/推流 5 槽），每级流水独立一份。
- **写满即丢最旧帧**，并通过 `abandon_function` 回调收回资源——硬解路径在这里 `mpp_frame_deinit`，防止 MPP 缓冲泄漏。
- 提供 **阻塞消费** `get_new()`（条件变量等待）与 **非阻塞消费** `try_get_new()`（无帧立即返回、按积压量取舍），
  供下游在高负载下主动让出或丢帧。

### 硬件解码与 RGA 缩放

- **MPP 硬解**：H.265 / H.264 走 RK 硬件解码器，输出 NV12 的 `MppFrame`（含 `buf_fd` / stride / 物理地址）。
- **RGA 转色 + 缩放**：`cvtcolor`（NV12→RGB）+ `resize`（到模型输入尺寸）在 RGA 硬件上完成，
  通过 `mpp_buffer_inc_ref` / `mpp_buffer_put` 精确管理硬解缓冲生命周期。

### RKNN 推理（`rkn_0.0.0`）

- `rknn_create_mem` 分配 I/O 张量内存，`rknn_set_io_mem` 绑定，
  `rknn_mem_sync` 在 CPU 与 NPU 间同步，输入输出零拷贝。

### YOLOv8 后处理（`yolov8_0.0.0.cc`）

- **NEON 阈值筛选**：以 int8 直接比较置信度（`vcgeq_s8`），产出位掩码后用 `ctz` 逐位枚举命中格点，跳过所有未命中项。
- **LUT 反量化**：`dequantiz_table` / `dequantiz_sigmoid_table` / `dequantiz_exp` 三张三查表；
  plane A 的 DFL softmax 直接用 exp 表算（并已验证 box logits 范围 < 20，可直接查表、无需减最大值，兼顾数值稳定）。
- **按类 NMS**：先按「类别 + 置信度」排序并切出每类区间，再在类内做 IoU 抑制，避免跨类比较。

### YOLOv5 后处理（`yolov5_0.0.0.cc`）

- **三头并行解码**：三个输出头各一个 worker 线程，条件变量令牌式同步（`dispatch_and_wait`），每帧并行解码后归并。
- **NEON NMS**：检测框先转 **SoA** 布局，NMS 用 `float32x4` 四路并行算交并比，结果写回 **位图**（`uint32_t` 掩码）批量剔除。
- **自绘中文标签**：`font_cn.h` 点阵字库直接往 RGB 帧缓冲写中文类别名。

### USE 观测（`observer.hh` + `use_check.sh`）

- 分别统计 RGA / VPU / NPU 的 **U（利用率）、S（流水线缓存积压）、E（错误计数）**，终端渲染成表格。
- `internal` 模式由软件计时采点，`external` 模式从内核节点读取（`/sys/kernel/debug/rknpu/load`、`/sys/kernel/debug/rkrga/load`、`/proc/mpp_service/load`）；可在启动时开关。

---

## 编译与运行

### 1. 交叉编译（PC 上）

`build.sh` 用 aarch64 交叉工具链把 `run.cc` + `any_source/{http,yolov8}*.cc` 编成板端可执行文件，
并自动生成 `running/`（可执行、依赖 `.so`、`set.cfg`、模型、启动脚本）。

```bash
./build.sh <模型路径>
# 例: ./build.sh ./yolov8s_normal_800448_plan_A.rknn
```

> ⚠️ `build.sh` 顶部的 `GXX / SYSROOT / PROJECT_ROOT` 是写死的本机路径，换环境时请改成你自己的交叉工具链与工程根目录。

### 2. 板端直接运行

```bash
cd running
./run_devices.sh
```
`run_devices.sh`：扫描所有以太网口 → 按摄像头网段自动配对网口并配置 IP → 逐路 ping 校验连通性 → 启动程序。

### 3. PC 端远程部署运行（需要 adb）

```bash
./run_pc.sh
```
流程：`adb push running/ → 开发板` → 板端自动配网 → `adb forward` 逐路端口转发 → 远程启动。

### 4. 查看画面

浏览器打开 `http://<板子IP或localhost>:8080`（第 2 路 8081，依次递增）。

---

## `set.cfg` 配置说明

```ini
stream_num=1              # 摄像头路数 (1-5)
cam1_uri=rtsp://...       # 每路 RTSP 地址 cam1_uri ~ cam5_uri
model_path=./xxx.rknn     # RKNN 模型路径
video_w=1920              # 输入分辨率宽
video_h=1080              # 输入分辨率高
dst_w=800                 # 送 NPU 的推理宽
dst_h=448                 # 送 NPU 的推理高
search_extimate=0         # YOLOv8 置信度筛选阈值（int8 域）
iou_extimate=0.5          # NMS 的 IoU 阈值
http_port=8080            # HTTP 起始端口（逐路 +1）
debug=quiet               # noisy(详细日志) / quiet(安静)
```

---

## YOLOv8 后处理：plan A / plan B

RKNN 量化时，YOLOv8 原模型末端把 `box` 与 `sort` 两个分支 `Concat` 成单输出，
但二者量化参数（scale / zero-point）差异极大（box 量程约 0–800，sort 经 sigmoid 后仅 0–1），
共用同一套量化参数会导致 **sort 分支精度严重坍缩**。因此本工程使用**裁剪掉 Concat 的双输出模型**：

- **plan A**：去掉 `Concat` 且拔掉 `box` 分支的 DFL 解码，把 DFL(softmax) 放回 **CPU** 软处理。
  - CPU 侧可先按 sort 分支筛选候选再做 DFL，计算量大幅减小；
  - box 裁剪到 logits 量程（约 ±16），比原 scale（~3.x）分辨率更细，量化精度更好。
- **plan B**：仅去掉 `Concat`，保留 `box` 分支的 DFL 在模型内。

后处理统一走「反量化表」法（见上）。裁剪工具见 `fix_a_auto.py`：自动识别 grid / 类别数，剪掉整条 DFL 链，
输出 `dfl_logits(1,64,N)` 与 `scores(1,nc,N)` 双输出，并内置数值验证（参考解码 vs 原模型，误差断言 < 1e-3）。

```bash
python fix_a_auto.py <in.onnx> [out.onnx]
```

---

## 分析文件说明

- `plane_A.txt` / `plane_B.txt`：RKNN Toolkit 的**逐层量化精度分析**，
  给出每层 simulator / runtime 的 `cos`（余弦相似度，越接近 1 越好）与 `euc`（欧氏误差）。
- `evel_planA`：RKNN Toolkit 的**性能/内存分析**，含模型内存占用（Weight / Internal Tensor / Total）、各频率设定，
  以及逐层 `OpType / Target / 耗时(us) / MacUsage` 表。

---

## 依赖

板端运行需以下组件（`build.sh` 会从 sysroot 拷贝到 `running/lib/`）：

- RKNN Runtime（`librknnrt.so`）
- Rockchip MPP（`librockchip_mpp.so`，硬解）
- RGA（`librga.so`，缩放/转色）
- GStreamer 1.0 + gst-plugins-base/app（`libgstreamer-1.0` / `libgstapp-1.0` / `libgstbase-1.0` / `libglib` / `libgobject` / `libgio`）
- OpenCV 4.14（`core / imgproc / imgcodecs`）

## 工具链

- 交叉编译器：`gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu`（aarch64）
- 目标 CPU：Cortex-A55（`-mcpu=cortex-a55`）
- 标准：C++17

---

## 已知注意事项

- `set.cfg` 中的摄像头 RTSP 地址**含示例账号口令，请替换为你自己的**再运行/提交。
- `build.sh` / `run_pc.sh` 中的路径（工具链、`PROJECT_ROOT`、板端部署目录 `/home/rock/psq/video_haikang`）为示例，按实际环境修改。
- 仓库提交时已排除编译产物（`.o`、`build/`、预编译可执行文件）与依赖 `.so`，请在构建机上自行生成。

---

## License

未指定。如需开源请补充 License；模型与第三方库（RKNN / MPP / RGA / GStreamer / OpenCV）各自受其原始许可约束。
