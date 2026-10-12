#!/bin/bash

# ======================== run_devices.sh ========================
# 运行于开发板上，负责：
#   1. 扫描所有以太网口，自动匹配摄像头网段 → 配 IP
#   2. 验证每路摄像头连通性
#   3. 启动 infer pipeline
#
#   ★ 支持：多路摄像头 / 多网段 / 多网口
# ===============================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CFG_FILE="$SCRIPT_DIR/set.cfg"
EXE_NAME="set_cam_1.1"

if [ ! -f "$CFG_FILE" ]; then
    echo "错误: set.cfg 不存在 → $CFG_FILE"
    exit 1
fi

if [ ! -f "$SCRIPT_DIR/$EXE_NAME" ]; then
    echo "错误: $EXE_NAME 不存在"
    exit 1
fi

# ======================== 1. 解析 set.cfg ========================
echo "===== 1. 解析 set.cfg ====="

CFG_CONTENT=$(grep -v '^[[:space:]]*#' "$CFG_FILE")

STREAM_NUM=$(echo "$CFG_CONTENT" | grep -oP '^stream_num=\K\d+' || echo "1")
HTTP_PORT=$( echo "$CFG_CONTENT" | grep -oP '^http_port=\K\d+'  || echo "8080")
MODEL_PATH=$(echo "$CFG_CONTENT" | grep -oP '^model_path=\K.*'   || echo "")

CAM_IPS=($( echo "$CFG_CONTENT" | grep -oP 'rtsp://[^@]+@\K\d+\.\d+\.\d+\.\d+' || true))
CAM_URIS=($(echo "$CFG_CONTENT" | grep -oP '^cam\d+_uri=\K.*' || true))

if [ ${#CAM_IPS[@]} -eq 0 ]; then
    echo "  未找到摄像头 IP，请检查 set.cfg"
    exit 1
fi

echo "  摄像头路数: $STREAM_NUM"
echo "  HTTP 起始口: $HTTP_PORT"
echo "  model_path: ${MODEL_PATH:-未配置}"
for i in $(seq 0 $((${#CAM_IPS[@]} - 1))); do
    echo "  第 $((i+1)) 路: ${CAM_IPS[$i]}  (${CAM_URIS[$i]})"
done

# ---- 提取唯一网段 ----
declare -A SUBNETS_MAP
for ip in "${CAM_IPS[@]}"; do
    subnet=$(echo "$ip" | sed 's/\.[0-9]\+$//')
    SUBNETS_MAP[$subnet]=1
done
CAM_SUBNETS=(${!SUBNETS_MAP[@]})

# ======================== 2. 扫描以太网口 ========================
echo ""
echo "===== 2. 扫描以太网口 ====="

ALL_IFACES=($(ip -o link show | awk -F': ' '{print $2}' | grep -E '^(eth|enp|ens|end)' | sort))
if [ ${#ALL_IFACES[@]} -eq 0 ]; then
    ALL_IFACES=($(ip -o link show | awk -F': ' '$2!="lo" {print $2}' | grep -v 'docker\|veth\|br-\|virbr\|wlan\|wlp' | sort))
fi

CARRIER_IFACES=()
NO_CARRIER_IFACES=()

for iface in "${ALL_IFACES[@]}"; do
    STATE=$(cat "/sys/class/net/$iface/operstate" 2>/dev/null || echo "unknown")
    CARR=$(cat  "/sys/class/net/$iface/carrier"  2>/dev/null || echo "?")
    IFACE_IP=$(ip -4 addr show "$iface" 2>/dev/null | grep -oP 'inet \K[\d.]+' | head -1)
    echo "    $iface  state=$STATE  carrier=$CARR  ip=${IFACE_IP:-无}"

    if [ "$CARR" = "1" ]; then
        CARRIER_IFACES+=("$iface")
    else
        NO_CARRIER_IFACES+=("$iface")
    fi
done

# ======================== 3. 网段 → 网口配对 & 配 IP ========================
echo ""
echo "===== 3. 网段 → 网口配对 ====="
echo "  摄像头网段: ${CAM_SUBNETS[*]}"

if [ ${#CARRIER_IFACES[@]} -eq 0 ]; then
    echo "  没有 carrier-up 的网口，尝试直接 ping..."
fi

declare -A ASSIGNED_SUBNETS
declare -A PING_CACHE

# 函数：尝试从某个网口 ping 指定 IP
try_ping() {
    local iface="$1" ip="$2"
    ping -c 2 -W 1 -I "$iface" "$ip" > /dev/null 2>&1 && return 0 || return 1
}

for subnet in "${CAM_SUBNETS[@]}"; do
    ASSIGNED=0

    # 优先找已有同网段 IP 的 carrier 口
    for iface in "${CARRIER_IFACES[@]}"; do
        [[ -n "${ASSIGNED_SUBNETS[$iface]}" && "${ASSIGNED_SUBNETS[$iface]}" != "$subnet" ]] && continue

        CUR_IP=$(ip -4 addr show "$iface" 2>/dev/null | grep -oP 'inet \K[\d.]+' | head -1)
        if [ -n "$CUR_IP" ] && echo "$CUR_IP" | grep -q "^$subnet"; then
            # 验证能 ping 通
            CAM_IP_FOR_SUBNET="${CAM_IPS[0]}"
            for ip in "${CAM_IPS[@]}"; do
                [ "$(echo "$ip" | sed 's/\.[0-9]\+$//')" = "$subnet" ] && CAM_IP_FOR_SUBNET="$ip" && break
            done
            if try_ping "$iface" "$CAM_IP_FOR_SUBNET"; then
                echo "  [$subnet] $iface 已有 $CUR_IP ✓ 连通"
                ASSIGNED_SUBNETS[$iface]="$subnet"
                ASSIGNED=1
                break
            else
                echo "  [$subnet] $iface 已有 $CUR_IP ✗ 不通，将重新配置"
            fi
        fi
    done
    [ "$ASSIGNED" -eq 1 ] && continue

    # 没找到 → 给一个空闲 carrier 口配 IP
    for iface in "${CARRIER_IFACES[@]}"; do
        [[ -n "${ASSIGNED_SUBNETS[$iface]}" && "${ASSIGNED_SUBNETS[$iface]}" != "$subnet" ]] && continue

        NEW_IP="${subnet}.100"
        echo "  [$subnet] $iface → $NEW_IP"

        # ★ flush 所有网口上此网段的 IP（防路由冲突）
        for i in "${ALL_IFACES[@]}"; do
            ip addr show "$i" 2>/dev/null | grep -q "$subnet" && ip addr flush dev "$i" 2>/dev/null
        done
        ip link set "$iface" up 2>/dev/null || true
        ip addr add "$NEW_IP/24" dev "$iface" 2>/dev/null || true
        sleep 1

        ASSIGNED_SUBNETS[$iface]="$subnet"
        ASSIGNED=1
        break
    done
    [ "$ASSIGNED" -eq 1 ] && continue

    # 还没找到 → 尝试从无 carrier 的网口
    for iface in "${NO_CARRIER_IFACES[@]}"; do
        NEW_IP="${subnet}.100"
        echo "  [$subnet] $iface (no-carrier) → $NEW_IP"
        for i in "${ALL_IFACES[@]}"; do
            ip addr show "$i" 2>/dev/null | grep -q "$subnet" && ip addr flush dev "$i" 2>/dev/null
        done
        ip link set "$iface" up 2>/dev/null || true
        ip addr add "$NEW_IP/24" dev "$iface" 2>/dev/null || true
        sleep 1
        ASSIGNED_SUBNETS[$iface]="$subnet"
        ASSIGNED=1
        break
    done

    if [ "$ASSIGNED" -eq 0 ]; then
        echo "  [$subnet] ✗ 无可用网口"
    fi
done

# ======================== 4. 连通性验证 ========================
echo ""
echo "===== 4. 连通性验证 ====="

ALL_OK=1
for i in $(seq 0 $((${#CAM_IPS[@]} - 1))); do
    ip="${CAM_IPS[$i]}"
    subnet=$(echo "$ip" | sed 's/\.[0-9]\+$//')

    FOUND_IFACE=""
    for iface in "${!ASSIGNED_SUBNETS[@]}"; do
        [ "${ASSIGNED_SUBNETS[$iface]}" = "$subnet" ] && FOUND_IFACE="$iface" && break
    done
    [ -z "$FOUND_IFACE" ] && FOUND_IFACE="${CARRIER_IFACES[0]}"

    echo -n "  cam$((i+1)) $ip → via $FOUND_IFACE ... "
    if try_ping "$FOUND_IFACE" "$ip"; then
        echo "✓"
    else
        echo "✗"
        ALL_OK=0
    fi
done

if [ "$ALL_OK" -eq 0 ]; then
    echo ""
    echo "  [警告] 部分摄像头不通，程序启动后可能无画面"
    echo "  请检查: 摄像头通电 / 网线接对 / set.cfg 中 camX_uri 的 IP 正确"
fi

# ======================== 5. 启动程序 ========================
echo ""
echo "===== 5. 启动 $EXE_NAME ====="
BOARD_IP=""
for iface in "${!ASSIGNED_SUBNETS[@]}"; do
    BOARD_IP=$(ip -4 addr show "$iface" 2>/dev/null | grep -oP 'inet \K[\d.]+' | head -1)
    [ -n "$BOARD_IP" ] && break
done
[ -z "$BOARD_IP" ] && BOARD_IP="localhost"

for i in $(seq 0 $((STREAM_NUM - 1))); do
    PORT=$((HTTP_PORT + i))
    echo "  第 $((i+1)) 路 → http://${BOARD_IP}:$PORT"
done
echo ""
echo "按 Ctrl+C 停止"
echo ""

cd "$SCRIPT_DIR"
LD_LIBRARY_PATH=./lib ./$EXE_NAME ./set.cfg 2>&1