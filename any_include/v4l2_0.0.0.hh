/*
*@brief 用于v4l2的驱动接口封装，可以是usb摄像头
*@details
*上层总共有三种使用模式，分别是AUTO、RGA、USER || 多种视频格式：YUYV、JPEG、RGB3x8、NV12{单平面，多平面}、RGB4x8
*AUTO：也就是默认要v4l2驱动给的设备内存【暂未实现】
*rga； rga模式是上层需要把v4l2的输出转别的格式时用到，主要分配为cma内存，方便rga_v4l2.hh使用
*USER：用户模式，上层需要自己申请内存，主要分配为高地址段【暂未实现】
*
*@todo 目前只支持，v4l2.hh模式中的rga用法，单平面，其他格式不支持
* 
*/

#ifndef V4L2_HH
#define V4L2_HH
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <cstdint>
#include <errno.h>

//========== 枚举定义 ==========：
/*
*@brief 视频格式枚举，用于v4l2_set_format()中使用的视频格式
*/
enum VIDEO_FORMAT
{
    YUYV = V4L2_PIX_FMT_YUYV,
    JPEG = V4L2_PIX_FMT_JPEG,
    RGB3x8 = V4L2_PIX_FMT_RGB24,
    NV12 = V4L2_PIX_FMT_NV12,
    RGB4x8 = V4L2_PIX_FMT_RGB32,

};

/*
*@brief 计算视频格式的位深，也就是每个像素占用的位数
*@note 主要用于申请视频缓冲区的内存cma的内存分配
*/
static size_t _bpp(VIDEO_FORMAT fmt) {
    switch (fmt) {
        case YUYV: return 2;
        case RGB3x8:  return 3;
        case RGB4x8:  return 4;
        case NV12: return 1;
        case JPEG: return 0; // 变长，特殊处理
    }
    return 0;
}

/*
*@brief 用户模式枚举
*/
enum USER_MODE
{
    AUTO = 0,
    RGA = 1,
    USER = 2,

};


//========申请dma堆内存的结构体============
//申请dma堆内存的结构体，用来申请视频缓冲区的内存cma【rga模式会用到，因为rk3568上rga的DMA32位够不到高地址段】
struct dma_heap_allocation_data {
    __u64 len;
    __u32 fd;
    __u32 fd_flags;
    __u64 heap_flags;
};
#define DMA_HEAP_IOC_MAGIC      'H'
#define DMA_HEAP_IOCTL_ALLOC _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)


//========== 宏定义 ==========：
#define BUF_COUNT 4 //视频缓冲区数量
#define MAX_PLANES 3//最大平面数



//========== 结构体定义 ==========：

    //多平面视频缓冲区结构体
    struct several_buf {
        uint32_t num_planes;     // 1 / 2 / 3
        void    *vaddr[BUF_COUNT][MAX_PLANES];
        size_t   length[BUF_COUNT][MAX_PLANES];
        size_t   bytesused[BUF_COUNT][MAX_PLANES];
        uint32_t bytesperline[BUF_COUNT][MAX_PLANES];
    };

    //单平面视频缓冲区结构体
    struct alone_buf {
        void    *vaddr[BUF_COUNT];
        size_t   length[BUF_COUNT];
        size_t   bytesused[BUF_COUNT];
        uint32_t bytesperline[BUF_COUNT];
        // 单平面 NV12 专用
        size_t   y_offset;
        size_t   uv_offset;
    };

    //class_detail;
    struct rga_mode
    {
        const char* cma_heap;
        union//因为是cma内存，所以需要申请dma堆内存的文件描述符
        {
            int s_dma_fd[BUF_COUNT][MAX_PLANES];
            int a_dma_fd[BUF_COUNT];
        };
        
    };

    struct user_mode
    {
        void** bufs;
        size_t buf_number;
        size_t buf_size;
    };

    struct auto_mode
    {
        
    };

    //总结构体
struct v4l2_context {
    // --- 设备 ---
    int      video_fd;//视频设备文件描述符

    //--视频参数--
    uint32_t width;
    uint32_t height;
    uint32_t fps;

    // --- 驱动协商后的格式---
    enum USER_MODE mode_type;         // MODE_USER / MODE_RGA / MODE_AUTO
    enum VIDEO_FORMAT format;         // YUYV / JPEG / RGB / NV12
    bool is_alone;
    
    int capture;                     // CAPTURE 或 CAPTURE_MPLANE
    int memory_type;                  // V4L2_MEMORY_*
    int pixel_size;                   // 每个像素的字节数

    // --- buffer 数量 ---
    size_t buf_count = BUF_COUNT;     //想要的视频缓冲区数量
    uint32_t actual_count;            //驱动实际给的数量

    // --- 模式 ---
    union {
        struct rga_mode* rga;
        struct user_mode* user;
        struct auto_mode* v4l2auto;
    };

    // --- 视频缓冲区 ---
    union {
        struct alone_buf* alone;
        struct several_buf* several;
    };
};




//========== 类定义 ==========：
class PSQ_v4l2
{
public:
    //================构造函数=================

    /*
    *@brief 设备内存构造函数 auto模式
    *@todo 未实现
    */
    PSQ_v4l2(const char* device,VIDEO_FORMAT format,size_t width,size_t height,size_t fps,bool alone = true);


    /*
    *@brief 用户内存构造函数 user模式
    *@todo 未实现
    */
    PSQ_v4l2(const char* device,void** bufs,size_t buf_number,size_t buf_size,VIDEO_FORMAT format,size_t width,size_t height,size_t fps,bool alone = true);
    
    //v4l2自动构造函数【如果不需要驱动自己分配内存，且需要自动申请cma内存跑后续rga视频缓冲区】
    /*
    *@brief v4l2自动构造函数 【rga模式】
    *@param[0] const char* device 视频设备路径
    *@param[1] const char* cma_heap cma堆内存路径
    *@param[2] VIDEO_FORMAT format 视频格式
    *@param[3] size_t width 视频宽度
    *@param[4] size_t height 视频高度
    *@param[5] size_t fps 视频帧率
    *@param[6] bool alone 是否单平面视频缓冲区
    */
    PSQ_v4l2(const char* device, const char* cma_heap, VIDEO_FORMAT format, size_t width, size_t height, size_t fps,bool alone = true)
    {
        //初始化总结构体
        this->context.video_fd = open(device,  O_RDWR | O_NONBLOCK);
        this->context.width = width;
        this->context.height = height;
        this->context.fps = fps;
        this->context.mode_type = RGA;
        this->context.format = format;
        
        if(alone)
        {
            this->context.capture = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            this->context.is_alone = true;
            this->context.alone = new alone_buf();
        }
        else
        {
            this->context.capture = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            this->context.is_alone = false;
            this->context.several = new several_buf();
        }
        
        this->context.memory_type = V4L2_MEMORY_DMABUF;
        this->context.pixel_size = _bpp(this->context.format);

        //初始化rga结构体
        this->context.rga = new rga_mode();
        this->context.rga->cma_heap = cma_heap;//填写cma堆内存路径文件描述地址

        request_dma_heap(this->context.rga,this->context.alone);

        this->v4l2_common_init();

        if(alone)
        {
            this->rga_alone_v4l2_request_buffers();
            this->rga_alone_v4l2_start_stream();
        }
        else
        {
            this->rga_several_v4l2_request_buffers();
            this->rga_several_v4l2_start_stream();
        }
    }


    // --- 取帧 / 还帧 ---
    /*
    *@brief 取帧，返回视频区索引下标
    *@return int 视频区索引下标
    *@note 如果后面又rga后处理，可以直接拿着返回值给rga_v4l2.hh相关函数使用
    */
    int v4l2_out_queue_for_rga()//单多都一样
    {
        memset(this->buf, 0, sizeof(*this->buf));
        this->buf->type   = this->context.capture;
        this->buf->memory = this->context.memory_type;
        if (ioctl(this->context.video_fd, VIDIOC_DQBUF, this->buf) < 0) {
            if (errno == EAGAIN)
                return -2;//等帧
            perror("VIDIOC_DQBUF");
            return -1;
        }
        return this->buf->index;
    }

    void* v4l2_out_queue_alone(void** user_str)
    {   
        memset(this->buf, 0, sizeof(*this->buf));
        this->buf->type   = this->context.capture;
        this->buf->memory = this->context.memory_type;
        if (ioctl(this->context.video_fd, VIDIOC_DQBUF, this->buf) < 0) {
            if (errno == EAGAIN)
                return NULL;
            perror("VIDIOC_DQBUF");
            return NULL;
        }

        return this->context.alone->vaddr[this->buf->index];
    }



    /*
    *@brief 还帧
    *@return int 0 成功 -2 失败 -1 其他错误
    *@note 还帧时，不需要将视频区索引下标作为参数传入，类内部有存放buf结构体
    *@warning 后处理玩后，需要调用v4l2_join_queue()函数还帧
    */
    int v4l2_join_queue()
    {
        if (ioctl(this->context.video_fd, VIDIOC_QBUF, this->buf) < 0) {
            if (errno == EAGAIN)
                return -2;
            perror("VIDIOC_QBUF");
            return -1;
        }
        return 0;
    }


    /*
    *@brief 如果上层不需要rga模式，可以通过v4l2_out_queue()函数返回的下标，传给此函数返回对应的地址
    *@brief 根据 index 查询缓冲区信息（零分配、零拷贝）
    *@param[in] index 视频区索引下标
    *@return void* 视频区虚拟地址
    *@note 只有在rga模式下，才需要调用此函数
    */

    size_t get_length(int index)
    {
        if (this->context.is_alone)
            return this->context.alone->length[index];
        return 0;
    }

    uint32_t get_width()        { return this->context.width; }
    uint32_t get_height()       { return this->context.height; }
    uint32_t get_actual_count() { return this->context.actual_count; }


//================析构函数=================
    ~PSQ_v4l2()
    {
        if(this->context.video_fd >= 0)
        {
            int type = this->context.capture;
            ioctl(this->context.video_fd, VIDIOC_STREAMOFF, &type);
        }

        delete this->buf;
        this->buf = nullptr;

        if(this->context.is_alone && this->context.alone)
        {
            for(uint32_t i = 0 ; i < this->context.actual_count ; i++)
            {
                if(this->context.alone->vaddr[i] && this->context.alone->vaddr[i] != MAP_FAILED)
                    munmap(this->context.alone->vaddr[i], this->context.alone->length[i]);
                if(this->context.rga->a_dma_fd[i] >= 0)
                    close(this->context.rga->a_dma_fd[i]);
            }
            delete this->context.alone;
        }
        else if(!this->context.is_alone && this->context.several)
        {
            delete this->context.several;
        }

        delete this->context.rga;

        if(this->context.video_fd >= 0)
            close(this->context.video_fd);
    }


private:
//================私有成员=================
    struct v4l2_context context;
    struct v4l2_buffer* buf;    //出队入队中转站
//================成员函数=================

    int request_dma_heap(struct rga_mode* rga , struct alone_buf* buf)//只用rga用到的函数，申请视频缓冲区的内存cma
    {
        
        for (size_t i = 0; i < this->context.buf_count; i++)
        {
            int heap_fd = open(rga->cma_heap, O_RDWR);
            if (heap_fd < 0) {
                perror("open");
                return -1;
            }

            struct dma_heap_allocation_data alloc_data = {
                .len = this->context.width * this->context.height * this->context.pixel_size,
                .fd_flags = O_RDWR | O_CLOEXEC,
            };


            if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc_data) < 0) 
            {
                perror("DMA_HEAP_IOCTL_ALLOC");
                close(heap_fd);
                return -1;
            }
            close(heap_fd);

            rga->a_dma_fd[i] = alloc_data.fd;
            buf->length[i] = this->context.width * this->context.height * this->context.pixel_size;
            buf->vaddr[i]   = mmap(NULL, buf->length[i], PROT_READ | PROT_WRITE,
                                         MAP_SHARED, alloc_data.fd, 0);

            if (buf->vaddr[i] == MAP_FAILED)
            {
                perror("mmap dma_heap");
                close(alloc_data.fd);
                return -1;
            }
        }
        
        return 0;
    }
    int request_dma_heap(struct rga_mode* rga , struct several_buf* buf)
    {
        return 0;
    }

    int v4l2_query()
    {
        struct v4l2_capability cap;//用来获取视频设备的属性信息
        if (ioctl(this->context.video_fd, VIDIOC_QUERYCAP, &cap) < 0) {
            perror("VIDIOC_QUERYCAP");
            return -1;
        }
        printf("Camera: %s  Driver: %s\n", cap.card, cap.driver);
        return 0;
    }

    int v4l2_set_format()
    {
        struct v4l2_format fmt;//用来设置视频设备的格式
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = this->context.capture;//设置视频设备的格式为视频捕获（也就是说我们要得到视频数据）
        fmt.fmt.pix.width = this->context.width; //设置视频宽度的像素数
        fmt.fmt.pix.height = this->context.height; //设置视频高度的像素数
        fmt.fmt.pix.pixelformat = this->context.format; //设置视频像素格式为this->context.format
        fmt.fmt.pix.field = V4L2_FIELD_NONE; //设置视频场为无（也就是不拆分，获取完整的整帧数据）

        if (ioctl(this->context.video_fd, VIDIOC_S_FMT, &fmt) < 0) {
            perror("VIDIOC_S_FMT YUYV");
            return -1;
        }
        printf("Format: %c%c%c%c %ux%u  size=%u\n",
            (fmt.fmt.pix.pixelformat >> 0) & 0xFF,
            (fmt.fmt.pix.pixelformat >> 8) & 0xFF,
            (fmt.fmt.pix.pixelformat >> 16) & 0xFF,
            (fmt.fmt.pix.pixelformat >> 24) & 0xFF,
            fmt.fmt.pix.width, fmt.fmt.pix.height,
            fmt.fmt.pix.sizeimage);
    return 0;

}

    int v4l2_set_fps()
    {
        struct v4l2_streamparm parm;//用来设置视频设备的帧率
        memset(&parm, 0, sizeof(parm));
        parm.type = this->context.capture;//设置视频设备的帧率类型为视频捕获（也就是说我们要得到视频数据）
        parm.parm.capture.timeperframe.numerator = 1;//设置视频设备的帧率的分子为1
        parm.parm.capture.timeperframe.denominator = this->context.fps;//设置视频设备的帧率的分母为this->context.fps

        if (ioctl(this->context.video_fd, VIDIOC_S_PARM, &parm) < 0) {
            perror("VIDIOC_S_PARM");
            return -1;
        }

        printf("FPS: %u/%u\n",
            parm.parm.capture.timeperframe.denominator,
            parm.parm.capture.timeperframe.numerator);
        return 0;
    }

    /*
    *@brief 初始化视频设备
    *@return int 0 成功 -2 失败 -1 其他错误
    *@note 因为初始化只有单多平面差别，而v4l2_query()，v4l2_set_format()，v4l2_set_fps()都是一样的，所以这边做一个统一管理
    */
    int v4l2_common_init()
    {
        this->buf = new struct v4l2_buffer();

        int ret = this->v4l2_query();
        if(ret != 0)
        {
            printf("v4l2_query failed\n");
            return ret;
        }
        ret = this->v4l2_set_format();
        if(ret != 0)
        {
            printf("v4l2_set_format failed\n");
            return ret;
        }
        ret = this->v4l2_set_fps();
        if(ret != 0)
        {
            printf("v4l2_set_fps failed\n");
            return ret;
        }
        
        return 0;
    }


//=================  rga   alone   ====================

    int rga_alone_v4l2_request_buffers()
    {
        struct v4l2_requestbuffers req;//填表给视频设备申请视频缓冲区
        memset(&req, 0, sizeof(req));
        req.count = this->context.buf_count;//申请的视频缓冲区数量
        req.type = this->context.capture;//申请的视频缓冲区类型为视频捕获（也就是说我们要得到视频数据）
        req.memory = this->context.memory_type;

        if (ioctl(this->context.video_fd, VIDIOC_REQBUFS, &req) < 0) {//提交申请
            perror("VIDIOC_REQBUFS");
            return -1;
        }

        this->context.actual_count = req.count;
        printf("Buffers requested: %d, got: %d\n", req.count, this->context.actual_count);
        //rga模式到此结束
        //后续为user模式和aoto模式的
        return 0;


    }

    int rga_alone_v4l2_start_stream()
    {
        for (int i = 0; i < this->context.actual_count; i++) {
            struct v4l2_buffer buf;
            memset(&buf, 0, sizeof(buf));
            buf.type   = this->context.capture;
            buf.memory = this->context.memory_type;
            buf.index  = i;
            buf.m.fd   = this->context.rga->a_dma_fd[i];
            buf.length = this->context.alone->length[i];              // ← 缓冲区大小

            if (ioctl(this->context.video_fd, VIDIOC_QBUF, &buf) < 0) {
                perror("VIDIOC_QBUF");
                return -1;
            }
        }
        int type = this->context.capture;
        if (ioctl(this->context.video_fd, VIDIOC_STREAMON, &type) < 0) {
            perror("VIDIOC_STREAMON");
            return -1;
        }

        return 0;

    }






//以下未完工：
//=================  rga   several  ===================
    int rga_several_v4l2_request_buffers()
    {
        return 0;
    }
    int rga_several_v4l2_start_stream()
    {
        return 0;
    }
//=================  auto  alone    =================
    int auto_alone_v4l2_request_buffers()
    {
        return 0;
    }
    int auto_alone_v4l2_start_stream()
    {
        return 0;
    }
//=================  auto  serval    =================
    int auto_serval_v4l2_request_buffers()
    {
        return 0;
    }
    int auto_serval_v4l2_start_stream()
    {
        return 0;
    }
//================= user alone  模式  ==============
    int user_alone_v4l2_request_buffers()
    {
        return 0;
    }
    int user_alone_v4l2_start_stream()
    {
        return 0;
    }
//================= user serval  模式  ==============
    int user_serval_v4l2_request_buffers()
    {
        return 0;
    }
    int user_serval_v4l2_start_stream()
    {
        return 0;
    }


//友元
public:
    friend class PSQ_RgaVel2;













    

};



#endif