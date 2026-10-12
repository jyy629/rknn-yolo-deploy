#include <rga.hh>

PSQ_rga::PSQ_rga(PSQ_v4l2 *v4l2 , size_t EX_WIDTH , size_t EX_HEIGHT)
{
    int ret;
    printf("-----------rga初始化中----------------\n");
    this->v4l2 = v4l2;
    this->v4l2_len = this->v4l2->in_cma.dma_len[0];
    this->v4l2_WIDTH = this->v4l2->WIDTH;
    this->v4l2_HEIGHT = this->v4l2->HEIGHT;
    printf("正在填入v4l2字段..........\n");
    for(int i = 0; i < BUF_COUNT; i++) {
        this->v4l2_va[i] = this->v4l2->in_cma.dma_address[i];
        this->v4l2_fd[i] = this->v4l2->in_cma.dma_fd[i];

        this->v4l2_handle[i] = importbuffer_fd(this->v4l2_fd[i], this->v4l2_len);
        if(!this->v4l2_handle[i]) {
            perror("v4l2 importbuffer_fd");
            return;
        }
        this->v4l2_rga_buf[i] = wrapbuffer_handle(
            this->v4l2_handle[i], 
            this->v4l2_WIDTH, 
            this->v4l2_HEIGHT, 
            RK_FORMAT_YUYV_422
        );
    }

    printf("正在填入rga字段..........\n");
    this->WIDTH = EX_WIDTH;
    this->HEIGHT = EX_HEIGHT;

    this->im_len  = this->WIDTH * this->HEIGHT * 3;
    this->tmp_len = this->WIDTH * this->HEIGHT * 2;

    printf("正在申请rga输出dma-buf(RGB)..........\n");
    ret = this->dma_buf_alloc(&this->im_dma_fd, &this->im_dma_va, this->im_len);
    if(ret < 0) { perror("dma_buf_alloc RGB"); return; }

    printf("正在申请rga中间dma-buf(YUYV)..........\n");
    ret = this->dma_buf_alloc(&this->tmp_dma_fd, &this->tmp_dma_va, this->tmp_len);
    if(ret < 0) { perror("dma_buf_alloc YUYV"); return; }

    this->handle = importbuffer_fd(this->im_dma_fd, this->im_len);
    if(!this->handle) { perror("importbuffer_fd RGB"); return; }

    this->tmp_handle = importbuffer_fd(this->tmp_dma_fd, this->tmp_len);
    if(!this->tmp_handle) { perror("importbuffer_fd YUYV"); return; }

    this->rga_buf = wrapbuffer_handle(
        this->handle, this->WIDTH, this->HEIGHT, RK_FORMAT_RGB_888);

    this->rga_tmp_buf = wrapbuffer_handle(
        this->tmp_handle, this->WIDTH, this->HEIGHT, RK_FORMAT_YUYV_422);
    printf("rga申请完成!!!!!\n");
}


int PSQ_rga::dma_buf_alloc(int *out_fd, void **out_va, size_t size)
    {
        int heap_fd = open(this->cma_heap, O_RDWR);
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

int PSQ_rga::yuyv2rgb(int number) {
    IM_STATUS ret = imresize(this->v4l2_rga_buf[number], this->rga_tmp_buf);
    if (ret != IM_STATUS_SUCCESS) return -1;

    ret = imcvtcolor(this->rga_tmp_buf, this->rga_buf,
                     RK_FORMAT_YUYV_422, RK_FORMAT_RGB_888);
    if (ret != IM_STATUS_SUCCESS) return -1;
    return 0;
}

    PSQ_rga::~PSQ_rga() {
        printf("~rga 析构: 释放 RGA buffer...\n");
        for (int i = 0; i < BUF_COUNT; i++) {
            releasebuffer_handle(this->v4l2_handle[i]);
        }
        releasebuffer_handle(this->handle);
        releasebuffer_handle(this->tmp_handle);

        if (this->im_dma_va && this->im_dma_va != MAP_FAILED) {
            munmap(this->im_dma_va, this->im_len);
        }
        if (this->im_dma_fd > 0) { close(this->im_dma_fd); }

        if (this->tmp_dma_va && this->tmp_dma_va != MAP_FAILED) {
            munmap(this->tmp_dma_va, this->tmp_len);
        }
        if (this->tmp_dma_fd > 0) { close(this->tmp_dma_fd); }
        printf("~rga 析构完成\n");
    }