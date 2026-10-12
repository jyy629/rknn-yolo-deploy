#ifndef CREAT_CMA_H
#define CREAT_CMA_H



const char *_cma_heap =  "/dev/dma_heap/reserved";



#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/types.h>


/* ---------- 自定义 dma-heap 结构体（不依赖内核头文件） ---------- */
#ifndef DMA_HEAP_ALLOCATION_DATA_DEFINED
#define DMA_HEAP_ALLOCATION_DATA_DEFINED
struct dma_heap_allocation_data {
    __u64 len;
    __u32 fd;
    __u32 fd_flags;
    __u64 heap_flags;
};
#endif
#define DMA_HEAP_IOC_MAGIC      'H'
#define DMA_HEAP_IOCTL_ALLOC _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)



int dma_buf_alloc(int *out_fd, void **out_va, size_t size)
{
    int heap_fd = open(_cma_heap, O_RDWR);
    if (heap_fd < 0) { perror("open heap"); return -1; }

    struct dma_heap_allocation_data data;
    memset(&data, 0, sizeof(data));
    data.len = size;
    data.fd_flags = O_RDWR | O_CLOEXEC;

    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0) {
        perror("ioctl alloc"); close(heap_fd); return -1;
    }
    close(heap_fd);

    *out_va = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, data.fd, 0);
    if (*out_va == MAP_FAILED) {
        perror("mmap"); close(data.fd); return -1;
    }
    *out_fd = data.fd;
    return 0;
}



int dma_buf_alloc(int heap_fd,size_t size)
{

    struct dma_heap_allocation_data data;
    memset(&data, 0, sizeof(data));
    data.len = size;
    data.fd_flags = O_RDWR | O_CLOEXEC;

    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0) {
        perror("ioctl alloc");  return -1;
    }

    
    return data.fd;
}

int dma_buf_alloc(size_t size)
{
    int heap_fd = open(_cma_heap, O_RDWR);
    if (heap_fd < 0) { perror("open heap"); return -1; }

    struct dma_heap_allocation_data data;
    memset(&data, 0, sizeof(data));
    data.len = size;
    data.fd_flags = O_RDWR | O_CLOEXEC;

    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0) {
        perror("ioctl alloc"); close(heap_fd); return -1;
    }
    close(heap_fd);
 
    return data.fd;
}






#endif