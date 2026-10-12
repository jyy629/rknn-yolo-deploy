# RKNN 端侧实时目标检测部署 Demo（YOLOv8 / YOLOv5）

面向 **Rockchip RK3568** 的端侧多路实时目标检测部署工程。完整跑通
**RTSP 拉流 → 硬件解码 → RGA 缩放/转色 → RKNN NPU 推理 → 后处理 → HTTP 推流** 全链路，
并附带模型裁剪工具与量化精度/性能分析。

> 语言：C++17（板端推理管线）+ Bash（交叉编译 / 部署）+ Python（ONNX 裁剪工具）

---

## 特性

- **多路并发**：1–5 路 RTSP 摄像头，每路独立线程与独立 HTTP 端口（8080 起递增）
- **全硬件流水线**：GStreamer 拉流 → Rockchip MPP 硬解（H.265 / H.264）→ RGA 缩放转色 → RKNN NPU 推理
- **连续内存 + 零拷贝设计**：DMA-Heap(CMA) 分配物理连续内存，通过 `rknn_set_io_mem` 直接绑定，避免额外拷贝
- **HTTP MJPEG 推流**：`multipart/x-mixed-replace`，浏览器直接打开 `http://<ip>:<port>` 即可看实时画面与检测框
- **YOLOv8 定制后处理**：针对 RKNN 量化特性深度裁剪的「双输出」模型（plan A / plan B），DFL 解码按需 CPU/NPU 分工
- **YOLOv5 后处理**：三输出 + anchor 解码，`pthread` 多线程并行 + NEON 加速 + NMS
- **USE 设备观测**：对 RGA / VPU / NPU 统计占用率（U / S / E），支持 internal / external 两种取数模式
- **一键构建与部署**：交叉编译脚本 + 板端自动配网启动脚本 + PC 端 adb 部署脚本

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
        └──────────────────── PSQ_strategic 管线调度（每级独立线程 + 缓存队列）────────────────────┘
                                        PSQ_observer (USE 监控)
```

每路摄像头对应一条独立队列，管线各级以线程 + 条件变量解耦，形成生产者—消费者流水。

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
│   ├── v4l2_0.0.0.hh       # V4L2 摄像头采集（本地 USB/MIPI 摄像头场景）
│   ├── rkn_0.0.0.hh        # RKNN 上下文 / 张量 / 内存封装
│   ├── yolov8_0.0.0.hh     # YOLOv8 后处理
│   ├── yolov5_0.0.0.hh     # YOLOv5 后处理
│   ├── http_0.0.0.hh       # HTTP MJPEG 服务
│   ├── observer.hh         # USE 利用率观测
│   ├── strategy.hh         # 管线调度（核心）
│   ├── creat_cma.h         # DMA-Heap 连续内存分配
│   └── sync_0.0.0.h
├── any_source/             # 上述模块的实现
│   ├── yolov8_0.0.0.cc
│   ├── yolov5_0.0.0.cc
│   └── http_0.0.0.cc
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

## 关键实现

- **管线调度 `PSQ_strategic`**：为拉流、解码、缩放、推理、推流各建线程，按 `video_id` 分流，
  使用 `std::mutex` + `condition_variable` 与环形缓存区解耦，支持优雅退出（`end_*` 依次收敛）。
- **连续内存 `creat_cma.h`**：自定义 `dma-heap` 结构，从 `/dev/dma_heap/reserved` 分配的物理连续内存，
  喂给 RGA / RKNN 时无需再做一次内存搬运。
- **RKNN 零拷贝**：`rknn_create_mem` + `rknn_set_io_mem` 绑定 I/O 张量内存，
  `rknn_mem_sync` 在 CPU 与 NPU 之间同步。
- **YOLOv8 后处理（`yolov8_0.0.0.*`）**：见下文。
- **YOLOv5 后处理（`yolov5_0.0.0.*`）**：反量化表法，多线程切分输出张量做并行解码，NEON 加速，最后统一 NMS。
- **USE 观测（`observer.hh` + `use_check.sh`）**：统计 RGA/VPU/NPU 的
  `U`（占用率）、`S`（状态）、`E`（错误/异常计数）并渲染成终端表格；`internal` 模式软件计时，`external` 模式从内核节点读取。

---

## 编译与运行

### 1. 交叉编译（PC 上）

`build.sh` 使用 aarch64 交叉工具链把 `run.cc` + `any_source/{http,yolov8}*.cc` 编成板端可执行文件，
并自动生成 `running/` 目录（含可执行、依赖 `.so`、`set.cfg`、模型、启动脚本）。

```bash
./build.sh <模型路径>
# 例: ./build.sh ./yolov8s_normal_800448_plan_A.rknn
```

> ⚠️ `build.sh` 顶部的 `GXX / SYSROOT / PROJECT_ROOT` 是写死的本机路径，
> 换环境时请改成你自己的交叉工具链与工程根目录。

### 2. 板端直接运行

```bash
cd running
./run_devices.sh
```
`run_devices.sh` 会：扫描所有以太网口 → 按摄像头网段自动配对网口并配置 IP → 逐路 ping 校验连通性 → 启动程序。

### 3. PC 端远程部署运行（需要 adb）

```bash
./run_pc.sh
```
流程：`adb push running/ → 开发板` → 在板端自动配网 → `adb forward` 逐路端口转发 → 远程启动。

### 4. 查看画面

浏览器打开 → `http://<板子IP或localhost>:8080`（第 2 路为 8081，依次递增）。

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
  - 好处：CPU 侧可先按 sort 分支筛选候选，再做 DFL，计算量大幅减小；
  - 且 box 裁剪到 logits 量程（约 ±16），比原 scale（~3.x）分辨率更细，量化精度更好。
- **plan B**：仅去掉 `Concat`，保留 `box` 分支的 DFL 在模型内。

后处理统一走「反量化表」法：预先按 `zero_point / scale` 构建 256 项查表
（`dequantiz_table` / `dequantiz_sigmoid_table` / `dequantiz_exp`），
运行时直接查表还原浮点，避免逐元素乘法。

裁剪工具见 `fix_a_auto.py`：自动识别 grid / 类别数，剪掉整条 DFL 链，输出
`dfl_logits(1,64,N)` 与 `scores(1,nc,N)` 双输出，并内置数值验证（参考解码 vs 原模型，误差断言 < 1e-3）。

```bash
python fix_a_auto.py <in.onnx> [out.onnx]
```

---

## 分析文件说明

- `plane_A.txt` / `plane_B.txt`：RKNN Toolkit 的**逐层量化精度分析**，
  给出每层 simulator / runtime 的 `cos`（余弦相似度，越接近 1 越好）与 `euc`（欧氏误差）。
- `evel_planA`：RKNN Toolkit 的**性能/内存分析**，包含模型内存占用
  （Weight / Internal Tensor / Total）、各频率设定，以及逐层 `OpType / Target / 耗时(us) / MacUsage` 表。

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
