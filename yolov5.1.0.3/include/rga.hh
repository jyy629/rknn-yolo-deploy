/*
该类用于对v4l2.hh采集的画面进行rgb，resize转换，同时交给yolorkn.hh进行识别

使用：
    创建实例后，调用yuyv2rgb函数，传入v4l2出队的序号，进行转换
    转换后的图片存在PSQ_rga.im_dma_va上，格式为rgba8888，大小为WIDTH*HEIGHT*4字节
*/

#ifndef RGA_H
#define RGA_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <rga/im2d.h>

#include "v4l2.hh"
#define BUF_COUNT 4

/* ========== 手写 dma-heap 结构体（不依赖系统头文件）========== */
typedef unsigned long long __u64;
typedef unsigned int __u32;



class PSQ_rga
{
public:
    friend class PSQ_yolov5_rk;

    PSQ_rga(PSQ_v4l2 *v4l2 , size_t EX_WIDTH , size_t EX_HEIGHT);

    //参数为v4l2出队的序号
    int yuyv2rgb(int number);

    int ex_dma_buf_alloc(const char *heap, size_t size, int *fd, void **va) 
    {
        int heap_fd = open(heap, O_RDWR);
        if (heap_fd < 0) { perror("open heap"); return -1; }

        struct dma_heap_allocation_data data;
        memset(&data, 0, sizeof(data));
        data.len = size;
        data.fd_flags = O_RDWR | O_CLOEXEC;
        
        //返回的 data.fd 就是 dma-buf fd
        if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0) {
            perror("ioctl alloc"); close(heap_fd); return -1;
        }
        close(heap_fd);

        *va = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, data.fd, 0);
        if (*va == MAP_FAILED) { perror("mmap"); close(data.fd); return -1; }

        *fd = data.fd;
        return 0;
    }

    ~PSQ_rga();

//--------------------------------------------------------------------------
//rga相关
    int im_dma_fd;
    void *im_dma_va;
    rga_buffer_handle_t handle;
    rga_buffer_t rga_buf;

    int tmp_dma_fd;
    void *tmp_dma_va;
    rga_buffer_handle_t tmp_handle;
    rga_buffer_t rga_tmp_buf;

    size_t im_len;
    size_t tmp_len;
    size_t WIDTH;
    size_t HEIGHT;


private:
    
    int dma_buf_alloc(int *out_fd, void **out_va, size_t size);

    const char *cma_heap = "/dev/dma_heap/reserved";
//--------------------------------------------------------------------------
//v4l2相关
    PSQ_v4l2 *v4l2;
    void *v4l2_va[BUF_COUNT];//v4l2的虚拟地址
    int v4l2_fd[BUF_COUNT];//v4l2的fd
    rga_buffer_handle_t v4l2_handle[BUF_COUNT];//v4l2的handle
    rga_buffer_t v4l2_rga_buf[BUF_COUNT];//v4l2的rga_buf
    size_t v4l2_len;//v4l2的dma-buf的长度（yuyv的）
    size_t v4l2_WIDTH;//v4l2的期望的宽度
    size_t v4l2_HEIGHT;//v4l2的期望的高度




};

#endif