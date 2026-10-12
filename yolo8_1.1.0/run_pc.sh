#!/bin/bash
set -e

# ======================== run_pc.sh ========================
# 运行于主机 PC，负责：
#   1. ADB 推送 running/ → 开发板
#   2. 自动检测摄像头网段，为每个网段找一个 carrier 网口并配 IP
#   3. 做所有摄像头的端口转发（8080, 8081, ...）
#   4. 远程启动程序
# ==========================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RUNNING_DIR="$SCRIPT_DIR/running"
DEVICE_DIR="/home/rock/psq/video_haikang"
EXE_NAME="set_cam_1.1"

if [ ! -d "$RUNNING_DIR" ]; then
    echo "错误: running/ 目录不存在，请先运行 ./build.sh"
    exit 1
fi

if ! command -v adb &> /dev/null; then
    echo "错误: 未找到 adb，请安装 Android SDK Platform Tools"
    exit 1
fi

# ---- 解析 set.cfg ----
CFG_FILE="$RUNNING_DIR/set.cfg"
CFG_CONTENT=$(grep -v '^[[:space:]]*#' "$CFG_FILE" 2>/dev/null || true)

HTTP_PORT=$( echo "$CFG_CONTENT" | grep -oP '^http_port=\K\d+' || echo "8080")
STREAM_NUM=$(echo "$CFG_CONTENT" | grep -oP '^stream_num=\K\d+' || echo "1")

# 提取每路摄像头的 IP 和 URI
CAM_IPS=($(echo "$CFG_CONTENT" | grep -oP 'rtsp://[^@]+@\K\d+\.\d+\.\d+\.\d+' || true))
CAM_URIS=($(echo "$CFG_CONTENT" | grep -oP '^cam\d+_uri=\K.*' || true))

if [ ${#CAM_IPS[@]} -eq 0 ]; then
    echo "错误: set.cfg 中未找到摄像头 RTSP 地址"
    exit 1
fi

# ---- 提取唯一网段 ----
declare -A SUBNETS
for ip in "${CAM_IPS[@]}"; do
    subnet=$(echo "$ip" | sed 's/\.[0-9]\+$//')
    SUBNETS[$subnet]=1
done
CAM_SUBNETS=(${!SUBNETS[@]})

# ======================== 1. 推送 running/ → 开发板 ========================
echo "===== 1. 推送 running/ → 开发板 ====="
adb push "$RUNNING_DIR/." "$DEVICE_DIR/" 2>&1
echo ""

# ======================== 2. 配置开发板以太网（按网段-网口配对） ========================
echo "===== 2. 配置开发板以太网 ====="
echo "  摄像头: ${STREAM_NUM} 路 / 网段: ${CAM_SUBNETS[*]}"

# 获取设备上所有 carrier-up 的网口
CARRIER_IFACES=($(adb shell "
    for iface in \$(ls /sys/class/net/ | grep -E '^(eth|enp|ens|end)'); do
        c=\$(cat /sys/class/net/\$iface/carrier 2>/dev/null || echo 0)
        [ \"\$c\" = \"1\" ] && echo \"\$iface\"
    done
" 2>/dev/null | tr -d '\r'))

if [ ${#CARRIER_IFACES[@]} -eq 0 ]; then
    echo "  [警告] 开发板上没有任何有物理链路的网口"
else
    echo "  可用网口: ${CARRIER_IFACES[*]}"
fi

# 为每个网段配一个 carrier 网口
IFACE_FOR_SUBNET=""
declare -A ASSIGNED_SUBNETS

for subnet in "${CAM_SUBNETS[@]}"; do
    ASSIGNED=0
    for iface in "${CARRIER_IFACES[@]}"; do
        # 此网口已被其他网段占用，跳过
        [[ -n "${ASSIGNED_SUBNETS[$iface]}" && "${ASSIGNED_SUBNETS[$iface]}" != "$subnet" ]] && continue

        NEW_IP="${subnet}.100"

        # 检查此网口当前 IP 是否匹配此网段
        CUR_IP=$(adb shell "ip -4 addr show $iface 2>/dev/null | grep -oP 'inet \K[\d.]+'" 2>/dev/null | tr -d '\r' | head -1)

        if [ -n "$CUR_IP" ] && echo "$CUR_IP" | grep -q "^$subnet"; then
            echo "  [$subnet] $iface 已有 $CUR_IP → 跳过"
        else
            # ★ 先从所有网口 flush 掉此网段的旧 IP，再重新配
            adb shell "
                for i in \$(ls /sys/class/net/ | grep -E '^(eth|enp|ens|end)'); do
                    ip addr show \$i 2>/dev/null | grep -q '$subnet' && ip addr flush dev \$i 2>/dev/null
                done
                ip link set $iface up 2>/dev/null
            " 2>/dev/null || true
            sleep 1

            adb shell "ip addr add $NEW_IP/24 dev $iface 2>/dev/null" 2>/dev/null || true
            sleep 1
            echo "  [$subnet] $iface → $NEW_IP"
        fi

        ASSIGNED_SUBNETS[$iface]="$subnet"
        IFACE_FOR_SUBNET="$iface"
        ASSIGNED=1
        break
    done

    if [ "$ASSIGNED" -eq 0 ]; then
        echo "  [$subnet] ✗ 无可用网口（所有 carrier 口已分配完毕）"
    fi
done

# 验证每路摄像头连通性
echo ""
echo "  --- 连通性验证 ---"
ALL_OK=1
for i in $(seq 0 $((${#CAM_IPS[@]} - 1))); do
    ip="${CAM_IPS[$i]}"
    subnet=$(echo "$ip" | sed 's/\.[0-9]\+$//')

    # 找到这个 IP 对应的网口
    FOUND_IFACE=""
    for iface in "${CARRIER_IFACES[@]}"; do
        if [ "${ASSIGNED_SUBNETS[$iface]}" = "$subnet" ]; then
            FOUND_IFACE="$iface"; break
        fi
    done
    [ -z "$FOUND_IFACE" ] && FOUND_IFACE="$IFACE_FOR_SUBNET"

    echo -n "  cam$((i+1)) $ip → via $FOUND_IFACE ... "
    if adb shell "ping -c 2 -W 1 -I $FOUND_IFACE $ip" > /dev/null 2>&1; then
        echo "✓"
    else
        echo "✗"
        ALL_OK=0
    fi
done

if [ "$ALL_OK" -eq 0 ]; then
    echo ""
    echo "  [警告] 部分摄像头 ping 不通，程序启动后可能无画面"
fi

echo ""

# ======================== 3. ADB 端口转发 ========================
echo "===== 3. 端口转发 ====="
for i in $(seq 0 $((STREAM_NUM - 1))); do
    PORT=$((HTTP_PORT + i))
    echo -n "  adb forward tcp:$PORT → 设备 tcp:$PORT ... "
    adb forward "tcp:$PORT" "tcp:$PORT" 2>/dev/null && echo "✓" || echo "✗"
done
echo ""

# ======================== 4. 启动程序 ========================
echo "===== 4. 启动 $EXE_NAME ====="
for i in $(seq 0 $((STREAM_NUM - 1))); do
    PORT=$((HTTP_PORT + i))
    echo "  第 $((i+1)) 路 → http://localhost:$PORT"
done
echo "  按 Ctrl+C 停止"
echo ""

adb shell "cd $DEVICE_DIR && LD_LIBRARY_PATH=./lib ./$EXE_NAME ./set.cfg" 2>&1