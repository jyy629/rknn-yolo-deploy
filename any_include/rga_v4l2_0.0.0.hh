/*
*@file    rga_vel2.hh

*@brief   专用于v4l2后处理为模型推理数据格式

*@note 只支持NHWC格式，但是需要rknn推理设置input_attr[0].pass_through = 1;让rknn内部转换为NCHW格式或其他格式

*@note 目前只支持，v4l2.hh模式中的rga用法，单平面

*@note rk3568的rga硬件对应的dma位宽只有32位，所以只能用cma中的内存，并且需要先设备，让cma固定低地址

* @version 1.0

* @date 2026-09-14

*/


#ifndef RGA_V4L2_HH
#define RGA_V4L2_HH


#include <v4l2_0.0.0.hh>
#include <rga/im2d.h>
/*
    @param[in] name 输入视频格式，和v4l2.hh中的enum VIDEO_FORMAT枚举对应一致
    @return 返回不同视频格式（YUYV/RGB3x8/RGB4x8/NV12/JPEG）对应的rga格式
*/
static size_t search_wrapbuffer(VIDEO_FORMAT fmt) {
    switch (fmt) {
        case YUYV: return RK_FORMAT_YUYV_422;
        case RGB3x8: return RK_FORMAT_RGB_888;
        case RGB4x8: return RK_FORMAT_RGBA_8888;
        case NV12: return 0;
        case JPEG: return 0; 
    }
    return 0;
}

/*
    @param[in] name 输入视频格式，和v4l2.hh中的enum VIDEO_FORMAT枚举对应一致
    @return 返回不同视频格式（YUYV/RGB3x8/RGB4x8/NV12/JPEG）对应的rga内存格式长度（字节）
*/
static size_t search_alloc_len(VIDEO_FORMAT fmt)
{
    switch (fmt) {
        case RGB3x8: return 3;
        case RGB4x8: return 4;
        case YUYV: return 2;
        case JPEG: return 0; 
        case NV12: return 0;

    }
    return 0;
}

/*
*@brief 目标信息结构体，用于存储目标视频的宽度、高度、通道格式、内存格式等信息
*@note 这些都是由上层传入构造函数
*/
struct destination_info
{
    size_t width;
    size_t height;
    
    //channel_format format;//(NHWC/NCHW)
    VIDEO_FORMAT fmt;//(YUYV/RGB3x8/RGB4x8/NV12/JPEG)

};


/*
*@brief 从v4l2.hh中获取信息的结构体，为了方便后续做rga转换而定义
*@brief 从v4l2获取的信息结构体【单平面模式】，用于存储v4l2申请的buffer数量、实际申请的buffer数量、dma-buf文件描述符、rga内存格式长度、v4l2申请的宽度、高度、格式等信息
*@note 这些都是由v4l2内部决定的，上层不需要关注
*@note 【没有太多实际作用，大部分原因是为了方便看清楚我们需要多少v4l2那边的信息】
*@note 只支持单平面模式，不支持多平面模式
*/
struct from_v4l2_info_alone//
{
    uint32_t v4l2_actual_count;
    int dma_fd[BUF_COUNT];
    size_t alone_rga_length[BUF_COUNT];
    size_t v4l2_width;
    size_t v4l2_height;
    enum VIDEO_FORMAT format;

    const char* cma_heap;

};

/*
*@brief 从v4l2获取的信息结构体【多平面模式】，用于存储v4l2申请的buffer数量、实际申请的buffer数量、dma-buf文件描述符、rga内存格式长度、v4l2申请的宽度、高度、格式等信息
*@todo 多平面模式下，需要根据v4l2申请的buffer数量，动态分配rga内存【待实现】
*/
struct from_v4l2_info_severral
{};


/*
*@brief 主要用于存放上层需求，然后代码更具这个需求来实现rga转换
*/
struct RgaImplement
{
    rga_buffer_handle_t v4l2_handle[BUF_COUNT];
    rga_buffer_t        v4l2_rga_buf[BUF_COUNT];

    rga_buffer_handle_t rga_handle;
    rga_buffer_t        rga_buf;
    void *rga_va;                   //用于后续返回给上层操作的存放数据的虚拟地址指针
    int rga_fd;
    VIDEO_FORMAT rga_fmt;



    rga_buffer_handle_t rga_tmp_handle;
    rga_buffer_t        rga_tmp_buf;
    void *rga_tmp_va;               //可要可不要，根据是否需要中间数据而定
    int rga_tmp_fd;
    VIDEO_FORMAT rga_tmp_fmt;

    size_t rga_len;                 //用于析构时munmap
    size_t rga_tmp_len;             //用于析构时munmap
};


class PSQ_RgaVel2
{

public:
    /*
    *@brief 构造函数，用于初始化rga转换
    *@param[in] ex_v4l2 v4l2对象指针
    *@param[in] expect_width 目标宽度
    *@param[in] expect_height 目标高度
    *@param[in] fmt 目标内存格式
    */
    PSQ_RgaVel2(PSQ_v4l2 *ex_v4l2, size_t expect_width, size_t expect_height,VIDEO_FORMAT fmt)
    {
        //填写目标信息
        {
            this->v4l2 = ex_v4l2;
            this->destination.width = expect_width;//目标宽度
            this->destination.height = expect_height;//目标高度
            //this->destination.format = format;//目标通道格式(NHWC/NCHW)
            this->destination.fmt = fmt;//目标内存格式(RGB3x8/RGB4x8/NV12/JPEG)
        }


        //初始化v4l2信息
        {
            this->from_v4l2.v4l2_actual_count = ex_v4l2->context.actual_count;//【实际申请的buffer数量（v4l2内部决定）】
            this->from_v4l2.v4l2_width = ex_v4l2->context.width;//【v4l2申请的宽度】
            this->from_v4l2.v4l2_height = ex_v4l2->context.height;//【v4l2申请的高度】
            this->from_v4l2.format = ex_v4l2->context.format;//【v4l2申请的格式】（yuyv/rgb/nv12/jpeg）
            this->from_v4l2.cma_heap = ex_v4l2->context.rga->cma_heap;//【cma堆地址】

            for(uint32_t i = 0; i < this->from_v4l2.v4l2_actual_count; i++)
            {
                this->from_v4l2.alone_rga_length[i] = ex_v4l2->context.alone->length[i];
                this->from_v4l2.dma_fd[i] = ex_v4l2->context.rga->a_dma_fd[i];//[存有用cma申请的文件描述符和cma文件地址]
            }
        }

        //将v4l2的文件描述符打包为rga的硬件格式
        for(uint32_t i = 0; i < this->from_v4l2.v4l2_actual_count; i++)
        {
            this->rga_implement.v4l2_handle[i] = importbuffer_fd(this->from_v4l2.dma_fd[i],this->from_v4l2.alone_rga_length[i]);
            if(!this->rga_implement.v4l2_handle[i]) 
            {
                perror("importbuffer_fd");
            }

            this->rga_implement.v4l2_rga_buf[i] = wrapbuffer_handle(
                this->rga_implement.v4l2_handle[i], this->from_v4l2.v4l2_width, this->from_v4l2.v4l2_height, search_wrapbuffer(this->from_v4l2.format));
        }


        //填写rga内部操作信息
            //1.先转尺寸到tmp          || 尺寸为【目标】尺寸 ， 格式为【v4l2源】格式
            //2.再从tmp转颜色到rga     || 尺寸为【目标】尺寸 ， 格式为【目标】格式
        
        this->rga_implement.rga_fmt =this->destination.fmt;
        this->rga_implement.rga_tmp_fmt = this->from_v4l2.format;
        
        //-----rga-----
        this->rga_implement.rga_len = this->destination.width * this->destination.height * search_alloc_len(this->rga_implement.rga_fmt);
        dma_buf_alloc(&this->rga_implement.rga_fd,&this->rga_implement.rga_va,this->rga_implement.rga_len);
        this->rga_implement.rga_handle = importbuffer_fd(this->rga_implement.rga_fd,this->rga_implement.rga_len);
        this->rga_implement.rga_buf = wrapbuffer_handle(
            this->rga_implement.rga_handle, this->destination.width, this->destination.height, search_wrapbuffer(this->rga_implement.rga_fmt));


        //-----rga_tmp-----
        this->rga_implement.rga_tmp_len = this->destination.width * this->destination.height * search_alloc_len(this->rga_implement.rga_tmp_fmt);
        dma_buf_alloc(&this->rga_implement.rga_tmp_fd,&this->rga_implement.rga_tmp_va,this->rga_implement.rga_tmp_len);
        this->rga_implement.rga_tmp_handle = importbuffer_fd(this->rga_implement.rga_tmp_fd,this->rga_implement.rga_tmp_len);
        this->rga_implement.rga_tmp_buf = wrapbuffer_handle(
            this->rga_implement.rga_tmp_handle, this->destination.width, this->destination.height, search_wrapbuffer(this->rga_implement.rga_tmp_fmt));

       
    }


    ~PSQ_RgaVel2()
    {
        for(uint32_t i = 0 ; i < this->from_v4l2.v4l2_actual_count ; i++)
        {
            if(this->rga_implement.v4l2_handle[i])
                releasebuffer_handle(this->rga_implement.v4l2_handle[i]);
        }

        if(this->rga_implement.rga_handle)
            releasebuffer_handle(this->rga_implement.rga_handle);
        if(this->rga_implement.rga_tmp_handle)
            releasebuffer_handle(this->rga_implement.rga_tmp_handle);

        if(this->rga_implement.rga_va && this->rga_implement.rga_va != MAP_FAILED)
            munmap(this->rga_implement.rga_va , this->rga_implement.rga_len);
        if(this->rga_implement.rga_fd >= 0)
            close(this->rga_implement.rga_fd);

        if(this->rga_implement.rga_tmp_va && this->rga_implement.rga_tmp_va != MAP_FAILED)
            munmap(this->rga_implement.rga_tmp_va , this->rga_implement.rga_tmp_len);
        if(this->rga_implement.rga_tmp_fd >= 0)
            close(this->rga_implement.rga_tmp_fd);


    }

    /*
    *@brief 转换函数，用于将v4l2的buffer转换为上层想要的格式
    *@note 主要实现尺寸转换和颜色转换，没有通道转换
    *@param[in] number v4l2申请的buffer下标索引
    *@return 0 成功
    *@return -1 失败
    *@todo 多平面模式下，需要根据v4l2申请的buffer数量，动态分配rga内存【待实现】
    */
    int convert(int number)//【number】为【v4l2申请的buffer下标索引】也就是出队函数的返回值
    {
        IM_STATUS ret = imresize(this->rga_implement.v4l2_rga_buf[number], this->rga_implement.rga_tmp_buf);
        if (ret != IM_STATUS_SUCCESS) return -1;

        ret = imcvtcolor(this->rga_implement.rga_tmp_buf, this->rga_implement.rga_buf,
                         search_wrapbuffer(this->rga_implement.rga_tmp_fmt), search_wrapbuffer(this->rga_implement.rga_fmt));
        if (ret != IM_STATUS_SUCCESS) return -1;
        return 0;
    }



    //================【新增】适配：公有接口=================
    size_t get_width()  const { return this->destination.width; }
    size_t get_height() const { return this->destination.height; }
    void*  get_rga_va() const { return this->rga_implement.rga_va; }


    
private:
    PSQ_v4l2 *v4l2;
    destination_info destination;
    from_v4l2_info_alone from_v4l2;
    RgaImplement rga_implement;



/*
 *@brief dma_buf_alloc函数，用于申请dma-buf
 *@param[in] out_fd dma-buf文件描述符指针
 *@param[in] out_va dma-buf虚拟地址指针
 *@param[in] size dma-buf大小
 *@return 0 成功
 *@return -1 失败
 */
int dma_buf_alloc(int *out_fd, void **out_va, size_t size)
    {
        int heap_fd = open(this->from_v4l2.cma_heap, O_RDWR);
        if(heap_fd < 0){ perror("open heap"); return -1; }

        struct dma_heap_allocation_data data;
        memset(&data, 0, sizeof(data));
        data.len = size;
        data.fd_flags = O_RDWR | O_CLOEXEC;

        if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0) {
            perror("ioctl alloc"); close(heap_fd); return -1;
        }
        close(heap_fd);

        *out_va = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, data.fd, 0);
        if (*out_va == MAP_FAILED) { perror("mmap"); close(data.fd); return -1; }
        *out_fd = data.fd;
        return 0;
    }

};






#endif