#ifndef RGA_HH
#define RGA_HH

enum RGA_FORMAT
{
    YUYV ,
    RGB3x8 ,
    RGB4x8  ,
    NV12  ,
    JPEG ,
};

#include <rga/im2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/types.h>

#include <sync_0.0.0.h>


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

/* ---------- RGA 硬件格式 映射 ---------- */
static int to_rk_format(RGA_FORMAT fmt) {
    switch (fmt) {
        case YUYV:   return RK_FORMAT_YUYV_422;
        case RGB3x8: return RK_FORMAT_RGB_888;
        case RGB4x8: return RK_FORMAT_RGBA_8888;
        case NV12:   return RK_FORMAT_YCbCr_420_SP;
        default:     return -1;
    }
}

/* ---------- 计算各格式的 buffer 字节数 ---------- */
static size_t rga_buf_bytes(RGA_FORMAT fmt, size_t w, size_t h) {
    switch (fmt) {
        case RGB3x8: return w * h * 3;
        case RGB4x8: return w * h * 4;
        case YUYV:   return w * h * 2;
        case NV12:   return w * h * 3 / 2;
        default:     return 0;
    }
}


/* =================================================================
 *  PSQ_rga  —  通用 RGA 硬件图像处理
 *
 *  管线（按实际需求顺序）:
 *
 *  源          cvtcolor()        tmp              resize()        dst
 *  ───        ─────────────     ──────────       ────────────    ─────────
 *  NV12       imcvtcolor       RGB3x8           imresize       RGB3x8
 *  1920×1080  ─────────────>   1920×1080  ──>                 640×640
 *  (src_fd)   NV12 → RGB      (原尺寸，画框用)  整图缩放      (模型输入)
 *
 *  内存:
 *    tmp_buf — 目标格式 × 源尺寸       构造时分配一次
 *    dst_buf — 目标格式 × 目标尺寸     构造时分配一次
 *    源 fd   — 外部传入，cvtcolor() 内临时 import / release
 *
 *  用法:
 *    PSQ_rga rga(NV12, 1920,1080,  RGB3x8, 640,640);
 *
 *    while (有帧) {
 *        rga.cvtcolor(mpp_fd);          // NV12 → RGB(原尺寸)
 *
 *        // 在 tmp_va() 上画框（1920×1080 RGB）
 *        draw_boxes(rga.tmp_va(), ...);
 *
 *        rga.resize();                   // 1920×1080 → 640×640
 *
 *        int rgb_fd = rga.dst_fd();     // 喂 RKNN
 *    }
 * ================================================================= */
class PSQ_rga
{
public:

    bool debug = false;

    PSQ_rga(RGA_FORMAT src_fmt, size_t src_w, size_t src_h,
            RGA_FORMAT dst_fmt, size_t dst_w, size_t dst_h)
        : _src_fmt(src_fmt), _src_w(src_w), _src_h(src_h),
          _dst_fmt(dst_fmt), _dst_w(dst_w), _dst_h(dst_h)
    {
        // ====== tmp: 目标格式 × 源尺寸（先转色、保留原分辨率，画框用） ======
        printf("==========【rga】初始化==========\n");
        _tmp_size = rga_buf_bytes(_dst_fmt, _src_w, _src_h);
        _tmp_fd   = -1;
        _tmp_va   = nullptr;
        if (_tmp_size > 0 && dma_buf_alloc(&_tmp_fd, &_tmp_va, _tmp_size) != 0) {
            printf("PSQ_rga: tmp buffer 分配失败\n");
            return;
        }
        _tmp_handle = importbuffer_fd(_tmp_fd, _tmp_size);
        if (!_tmp_handle) {
            printf("PSQ_rga: importbuffer_fd(tmp) 失败\n");
            return;
        }
        _tmp_buf = wrapbuffer_handle(_tmp_handle, _src_w, _src_h,
                                     to_rk_format(_dst_fmt));

        // ====== dst: 目标格式 × 目标尺寸（缩放后，模型输入） ======
        _dst_size = rga_buf_bytes(_dst_fmt, _dst_w, _dst_h);
        _dst_fd   = -1;
        _dst_va   = nullptr;
        if (_dst_size > 0 && dma_buf_alloc(&_dst_fd, &_dst_va, _dst_size) != 0) {
            printf("PSQ_rga: dst buffer 分配失败\n");
            return;
        }
        _dst_handle = importbuffer_fd(_dst_fd, _dst_size);
        if (!_dst_handle) {
            printf("PSQ_rga: importbuffer_fd(dst) 失败\n");
            return;
        }
        _dst_buf = wrapbuffer_handle(_dst_handle, _dst_w, _dst_h,
                                     to_rk_format(_dst_fmt));

        printf("PSQ_rga: init OK  %zux%zu(%d) -> %zux%zu(%d) -> %zux%zu(%d)\n",
               _src_w, _src_h, _src_fmt,
               _src_w, _src_h, _dst_fmt,
               _dst_w, _dst_h, _dst_fmt);
        printf("==========【rga】初始化完成==========\n");
    }


    ~PSQ_rga()
    {
        if (_dst_handle) releasebuffer_handle(_dst_handle);
        if (_tmp_handle) releasebuffer_handle(_tmp_handle);

        if (_dst_va && _dst_va != MAP_FAILED) munmap(_dst_va, _dst_size);
        if (_tmp_va && _tmp_va != MAP_FAILED) munmap(_tmp_va, _tmp_size);

        if (_dst_fd >= 0) close(_dst_fd);
        if (_tmp_fd >= 0) close(_tmp_fd);
    }


    /* ---- cvtcolor: src_fd（外部）→ tmp（目标格式 × 源尺寸） ----
     *   src_fd: 外部 dma-buf，用完不 close，调用者管理生命周期
     *   tmp:    目标格式 × 源尺寸，供画框使用
     */
    int cvtcolor(int src_fd)
    {
        rga_buffer_handle_t src_handle = importbuffer_fd(src_fd,
            rga_buf_bytes(_src_fmt, _src_w, _src_h));
        if (!src_handle) {
            printf("PSQ_rga::cvtcolor importbuffer_fd 失败\n");
            return -1;
        }

        rga_buffer_t src_buf = wrapbuffer_handle(src_handle, _src_w, _src_h,
                                                  to_rk_format(_src_fmt));

        IM_STATUS ret = imcvtcolor(
            src_buf, _tmp_buf,
            to_rk_format(_src_fmt),
            to_rk_format(_dst_fmt)
        );

        releasebuffer_handle(src_handle);
        dma_buf_sync(_tmp_fd, DMA_BUF_SYNC_READ);

        if (ret != IM_STATUS_SUCCESS) {
            printf("PSQ_rga::cvtcolor imcvtcolor 失败: %d\n", ret);
            return -1;
        }
        return 0;
    }


    //nv12_to_rgb
    int cvtcolor(int src_fd ,int hor_stride, int ver_stride, size_t src_size )
    {
        rga_buffer_handle_t src_handle = importbuffer_fd(src_fd,
            src_size);
        if (!src_handle) {
            printf("PSQ_rga::cvtcolor importbuffer_fd 失败\n");
            return -1;
        }

        //-----
        rga_buffer_t src_buf = wrapbuffer_handle(
            src_handle, 
            _src_w, 
            _src_h,
            to_rk_format(_src_fmt),
            hor_stride, 
            ver_stride
        );

        

        IM_STATUS ret = imcvtcolor(
            src_buf, _tmp_buf,
            to_rk_format(_src_fmt),      /* sfmt */
            to_rk_format(_dst_fmt)      /* dfmt */
        );  /* 系数 + 色域 */


        releasebuffer_handle(src_handle);
        dma_buf_sync(_tmp_fd, DMA_BUF_SYNC_READ);

        if (ret != IM_STATUS_SUCCESS) {
            printf("PSQ_rga::cvtcolor imcvtcolor 失败: %d\n", ret);
            return -1;
        }
        return 0;
    }


    /* ---- resize: tmp（目标格式 × 源尺寸）→ dst（目标格式 × 目标尺寸） ----
     *   调用 cvtcolor() 之后再调，全图缩放到模型输入尺寸
     */
    int resize()
    {
        IM_STATUS ret = imresize(_tmp_buf, _dst_buf);
        if (ret != IM_STATUS_SUCCESS) {
            printf("PSQ_rga::resize imresize 失败: %d\n", ret);
            return -1;
        }
        dma_buf_sync(_dst_fd, DMA_BUF_SYNC_READ);
        return 0;
    }


    /* ---- 一步到位（不需要画框时直接用） ---- */
    int convert(int src_fd)
    {
        if (cvtcolor(src_fd) != 0) return -1;
        if (resize()         != 0) return -1;
        return 0;
    }


    /* ---- tmp: 原尺寸 RGB，画框用 ---- */
    void  *tmp_va()   const { return _tmp_va; }
    int    tmp_fd()   const { return _tmp_fd; }
    size_t tmp_w()    const { return _src_w; }
    size_t tmp_h()    const { return _src_h; }
    size_t tmp_size() const { return _tmp_size; }

    /* ---- dst: 缩放后 RGB，模型输入用 ---- */
    void  *dst_va()   const { return _dst_va; }
    int    dst_fd()   const { return _dst_fd; }
    size_t dst_size() const { return _dst_size; }


private:

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


    RGA_FORMAT _src_fmt, _dst_fmt;
    size_t     _src_w,  _src_h;
    size_t     _dst_w,  _dst_h;

    /* ---- tmp: 目标格式 × 源尺寸（cvtcolor 输出，画框用） ---- */
    int                  _tmp_fd = -1;
    void                *_tmp_va = nullptr;
    size_t               _tmp_size = 0;
    rga_buffer_handle_t  _tmp_handle = 0;
    rga_buffer_t         _tmp_buf;

    /* ---- dst: 目标格式 × 目标尺寸（resize 输出，RKNN 输入） ---- */
    int                  _dst_fd = -1;
    void                *_dst_va = nullptr;
    size_t               _dst_size = 0;
    rga_buffer_handle_t  _dst_handle = 0;
    rga_buffer_t         _dst_buf;

    const char *_cma_heap = "/dev/dma_heap/reserved";
};

#endif