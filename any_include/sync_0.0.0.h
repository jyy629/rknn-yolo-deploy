#ifndef SYNC_0_0_0_H
#define SYNC_0_0_0_H

#include <string.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <asm-generic/int-ll64.h>
#include <stdio.h>
#include <unistd.h>
#define DMA_BUF_BASE   'b'

/* ---------- dma-buf cache 同步（Linux UAPI 标准定义，缺头文件时兜底） ---------- */
#ifndef DMA_BUF_SYNC_READ
struct dma_buf_sync {
    __u64 flags;
};
#define DMA_BUF_SYNC_READ   (1 << 0)   /* CPU 读 dma-buf 前：让硬件写的内容对 CPU 可见 */
#define DMA_BUF_SYNC_WRITE  (2 << 0)   /* CPU 写 dma-buf 后：把 CPU cache 刷给硬件读 */
#define DMA_BUF_SYNC_START  (0 << 2)
#define DMA_BUF_SYNC_END    (1 << 2)
#define DMA_BUF_IOCTL_SYNC  _IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)
#endif

/* 透传：调用方自行控制 START / END 时机 */
int dma_buf_sync(int fd, unsigned long flags)
{
    char target[256] = {0};
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    ssize_t len = readlink(path, target, sizeof(target) - 1);
    // printf("[dma_buf_sync] fd=%d ioctl=0x%08x flags=0x%lx target=%s\n",
    //        fd, (unsigned int)DMA_BUF_IOCTL_SYNC, flags,
    //        len > 0 ? target : "(unknown)");
    fflush(stdout);

    struct dma_buf_sync sync;
    memset(&sync, 0, sizeof(sync));
    sync.flags = flags;
    if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
        perror("dma_buf_sync");
        return -1;
    }
    return 0;
}


#endif