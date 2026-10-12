#!/bin/bash
# USE Monitor - 两种模式: internal(软件计时) / external(内核节点)
# 用法: ./use_check.sh

FILE=/tmp/use_monitor

while true; do
    clear

    if [ ! -f "$FILE" ]; then
        echo "====== USE Monitor ======"
        echo ""
        echo "  等待 /tmp/use_monitor ..."
        sleep 1
        continue
    fi

    unset mode ts
    unset rga_U_avg rga_U_now rga_U rga_S rga_E
    unset vpu_U_avg vpu_U_now vpu_U vpu_S vpu_E
    unset npu_U_avg npu_U_now npu_U npu_S npu_E

    source "$FILE" 2>/dev/null

    echo "====== USE Monitor [$mode] ======"
    echo ""

    if [ "$mode" = "internal" ]; then
        printf "┌────────┬──────────┬──────────┬───────┬───────┐\n"
        printf "│ Device │  U_avg   │  U_now   │   S   │   E   │\n"
        printf "├────────┼──────────┼──────────┼───────┼───────┤\n"
        printf "│  RGA   │ %6.1f%%  │ %6.1f%%  │ %5s │ %5s │\n" \
            "${rga_U_avg:-0}" "${rga_U_now:-0}" "${rga_S:-0}" "${rga_E:-0}"
        printf "│  VPU   │ %6.1f%%  │ %6.1f%%  │ %5s │ %5s │\n" \
            "${vpu_U_avg:-0}" "${vpu_U_now:-0}" "${vpu_S:-0}" "${vpu_E:-0}"
        printf "│  NPU   │ %6.1f%%  │ %6.1f%%  │ %5s │ %5s │\n" \
            "${npu_U_avg:-0}" "${npu_U_now:-0}" "${npu_S:-0}" "${npu_E:-0}"
        printf "└────────┴──────────┴──────────┴───────┴───────┘\n"
    else
        printf "┌────────┬──────────────┬───────┬───────┐\n"
        printf "│ Device │      U       │   S   │   E   │\n"
        printf "├────────┼──────────────┼───────┼───────┤\n"
        printf "│  RGA   │   %6.1f%%    │ %5s │ %5s │\n" \
            "${rga_U:--1}" "${rga_S:-0}" "${rga_E:-0}"
        printf "│  VPU   │   %6.1f%%    │ %5s │ %5s │\n" \
            "${vpu_U:--1}" "${vpu_S:-0}" "${vpu_E:-0}"
        printf "│  NPU   │   %6.1f%%    │ %5s │ %5s │\n" \
            "${npu_U:--1}" "${npu_S:-0}" "${npu_E:-0}"
        printf "└────────┴──────────────┴───────┴───────┘\n"
    fi

    sleep 1
done