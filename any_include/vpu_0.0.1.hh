#ifndef PSQ_VPU_HH
#define PSQ_VPU_HH

//========申请dma堆内存的结构体============
//申请dma堆内存的结构体，用来申请视频缓冲区的内存cma【rga模式会用到，因为rk3568上rga的DMA32位够不到高地址段】
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


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rk_mpi.h>
#include <rk_mpi_cmd.h>
#include <rk_vdec_cfg.h>
#include <mpp_buffer.h>
#include <mpp_frame.h>
#include <mpp_packet.h>
#include <mpp_err.h>
#include <vector>

enum vpu_decode_type
{
    H265 = MPP_VIDEO_CodingHEVC,
    H264 = MPP_VIDEO_CodingAVC,
};

struct after_vpu_frame_info
{
    MppBuffer mb = NULL;
    int hor_stride = 0;
    int ver_stride = 0;
    size_t data_size = 0;
    MppFrameFormat fmt;
    void* data = NULL;
    int buf_fd = 0;
    signed long long int pts = 0;
    MppFrame frm = NULL;
   
};

struct vpu_info
{
    MppCtx   ctx = NULL;
    MppApi  *api = NULL;
    MppFrame frm = NULL;
    MppBufferGroup grp = NULL;
};

class PSQ_vpu
{

public:

    bool debug;

    PSQ_vpu(vpu_decode_type type)
    {
        //==========创建环境==========
        printf("==========【vpu】初始化==========\n");
        MPP_RET ret = mpp_create(&info.ctx, &info.api);
        if (ret != MPP_OK) {
            printf("{构造函数}:: [mpp_create FAIL]error: ret=%d\n", ret);
            return;
        }

        //==========填写环境配置信息表==========
        MppDecCfg cfg = NULL;
        mpp_dec_cfg_init(&cfg);//初始化配置表
        mpp_dec_cfg_set_s32(cfg, "base:split_parse", 0);//问题1：是否需要设置为1？，前端gstreamer已经是完整au了（因为有h265parse元素）

        ret = info.api->control(info.ctx, MPP_DEC_SET_CFG, cfg); //写入表，逻辑类似ioctrl（）
        if (ret != MPP_OK) {
            printf("{构造函数}:: [mpp_control FAIL]error: ret=%d\n", ret);
            mpp_dec_cfg_deinit(cfg);
            mpp_destroy(info.ctx);
            return;
           
        }
        mpp_dec_cfg_deinit(cfg);//释放配置表




        //========== 打开vpu硬件 ==========
        if(type == H265)
        {
            ret = mpp_init(info.ctx, MPP_CTX_DEC, MPP_VIDEO_CodingHEVC);
        }
        else if(type == H264)
        {
            ret = mpp_init(info.ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC);
        }
        if (ret != MPP_OK) {
            printf("{构造函数}:: [mpp_init FAIL]error: ret=%d\n", ret);
            mpp_destroy(info.ctx);
            
        }
        printf("{构造函数}:: [all ok] (type=%d decoder ready)\n", type);

    }




    int send_to_vpu(void* data, size_t data_size)
    {
        MppPacket pkt = NULL;
        MPP_RET ret = mpp_packet_init(&pkt, data, data_size);
        if(ret != MPP_OK)
        {

            printf("【vpu】{send_to_vpu}:: [mpp_packet_init FAIL]  error: ret=%d\n", ret);
            
            
            return -1;
        }
        mpp_packet_set_pos(pkt, data);
        mpp_packet_set_length(pkt, data_size);
        mpp_packet_set_pts(pkt, m_pts_counter++);
        
        if(debug)
        {
            printf("【vpu】{send_to_vpu}::[proposal packet is readly]  number=%lld\n", m_pts_counter);
        }
        ret = info.api->decode_put_packet(info.ctx, pkt);
        if (ret != MPP_OK) {
            printf("【vpu】{send_to_vpu}:: [decode_put_packet()--send fail]  error: ret=%d\n", ret);
            return -1;
        }
        mpp_packet_deinit(&pkt);
        if(debug)
        {
            printf("【vpu】{send_to_vpu}:: [decode_put_packet()-- OK]\n");
        }

        return 0;
    }





    int get_data_form_vpu_model_1(after_vpu_frame_info* af_info, bool blocking)
    {
        MPP_RET ret;
        int times = blocking ? -1 : 8;

    retry:
        ret = info.api->decode_get_frame(info.ctx, &info.frm);
        if (ret == MPP_ERR_TIMEOUT) {
            if (blocking || times-- > 0) {
                usleep(1000);
                goto retry;
            }
        }
        if (ret != MPP_OK || info.frm == NULL)
        {
            info.frm = NULL;
            printf("【vpu】{get_data_form_vpu_model_1 (超时了！！)} error ret=%d\n", ret);
            return -1;
        }

    //--------------------------------
        if(mpp_frame_get_info_change(info.frm)!=0)
        {
            printf("【vpu】{get_data_form_vpu_model_1 (this is info_frame)} info_change w=%u h=%u\n",
                   mpp_frame_get_width(info.frm),
                   mpp_frame_get_height(info.frm));

            ret = info.api->control(info.ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
            if (ret != MPP_OK) {
                printf("【vpu】{get_data_form_vpu_model_1 (set info_change_ready fail)} error ret=%d\n", ret);
                mpp_frame_deinit(&info.frm);
                return -1;
            }

            mpp_frame_deinit(&info.frm);
            // 不 return，掉头取真帧
            return get_data_form_vpu_model_1(af_info, true);
        }
        

        af_info->mb = mpp_frame_get_buffer(info.frm);

        af_info->hor_stride = mpp_frame_get_hor_stride(info.frm);
        af_info->ver_stride = mpp_frame_get_ver_stride(info.frm);
        af_info->data_size = mpp_frame_get_buf_size(info.frm);
        af_info->fmt = mpp_frame_get_fmt(info.frm);
        af_info->data = mpp_buffer_get_ptr(af_info->mb);
        af_info->buf_fd = mpp_buffer_get_fd(af_info->mb);
        af_info->pts = mpp_frame_get_pts(info.frm);
  

        printf("【vpu】{get_data_form_vpu_model_1 (this is video_frame)} VIDEO FRAME  fd=%d  pts=%lld  stride=%dx%d\n",
        af_info->buf_fd, af_info->pts, af_info->hor_stride, af_info->ver_stride);

    
        return 0;

    }


    int  get_data_form_vpu_model_2(after_vpu_frame_info* af_info, bool blocking)
    {
        MPP_RET ret;
        int times = blocking ? -1 : 8;

    retry:
        ret = info.api->decode_get_frame(info.ctx, &info.frm);
        if (ret == MPP_ERR_TIMEOUT) {
            if (blocking || times-- > 0) {
                usleep(1000);
                goto retry;
            }
        }
        if (ret != MPP_OK || info.frm == NULL)
        {
            info.frm = NULL;
            printf("【vpu】{get_data_form_vpu_model_2 (超时了！！)} error ret=%d\n", ret);
            return -1;
        }

    //--------------------------------
        if(mpp_frame_get_info_change(info.frm)!=0)
        {
            printf(
                "【vpu】{get_data_form_vpu_model_2 (this is info_frame)} info_change w=%u h=%u\n",
                mpp_frame_get_width(info.frm),
                mpp_frame_get_height(info.frm)
            );
            

            af_info->data_size = mpp_frame_get_buf_size(info.frm);
            af_info->hor_stride = mpp_frame_get_hor_stride(info.frm);
            af_info->ver_stride = mpp_frame_get_ver_stride(info.frm);

            if(info.grp == NULL)
            {
                mpp_buffer_group_get_internal(&info.grp,MPP_BUFFER_TYPE_ION);
                info.api->control(info.ctx, MPP_DEC_SET_EXT_BUF_GROUP, info.grp);
            }

            mpp_buffer_group_limit_config(info.grp,af_info->data_size,24);

            info.api->control(info.ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
            if (ret != MPP_OK) {
                printf("【vpu】{get_data_form_vpu_model_2 (set info_change_ready fail)} error ret=%d\n", ret);
                mpp_frame_deinit(&info.frm);
                return -1;
            }


            mpp_frame_deinit(&info.frm);

            // 不 return，掉头取真帧
            return get_data_form_vpu_model_2(af_info, true);
        }
        

        af_info->mb = mpp_frame_get_buffer(info.frm);

        af_info->data_size = mpp_frame_get_buf_size(info.frm);
        af_info->hor_stride = mpp_frame_get_hor_stride(info.frm);
        af_info->ver_stride = mpp_frame_get_ver_stride(info.frm);
        af_info->fmt = mpp_frame_get_fmt(info.frm);
        af_info->data = mpp_buffer_get_ptr(af_info->mb);
        af_info->buf_fd = mpp_buffer_get_fd(af_info->mb);
        af_info->pts = mpp_frame_get_pts(info.frm);
  

        printf("【vpu】{get_data_form_vpu_model_2 (this is video_frame)} VIDEO FRAME  fd=%d  pts=%lld  stride=%dx%d\n",
        af_info->buf_fd, af_info->pts, af_info->hor_stride, af_info->ver_stride);

    
        return 0;

    }


    int get_data_from_vpu_model_3(after_vpu_frame_info* af_info, bool blocking)
    {
        MPP_RET ret;
        int times = blocking ? -1 : 8;

    retry:
        ret = info.api->decode_get_frame(info.ctx, &info.frm);
        if (ret == MPP_ERR_TIMEOUT || (ret == MPP_OK && info.frm == NULL)) {
            if (blocking || times-- > 0) {
                usleep(1000);
                goto retry;
            }
        }
        if (ret != MPP_OK || info.frm == NULL)
        {
            printf("【vpu】{get_data_form_vpu_model_3 (超时了！！)} error ret=%d\n", ret);
            return -1;
        }

    //--------------------------------
        if(mpp_frame_get_info_change(info.frm)!=0)
        {
            if(debug)
            {
                printf(
                    "【vpu】{get_data_from_vpu_model_3 (this is info_frame)} info_change w=%u h=%u\n",
                    mpp_frame_get_width(info.frm),
                    mpp_frame_get_height(info.frm)
                );
            }


            int heap_fd = open("/dev/dma_heap/reserved", O_RDWR);
            if (heap_fd < 0) { perror("open heap"); return -1; }

            af_info->data_size = mpp_frame_get_buf_size(info.frm);
            af_info->hor_stride = mpp_frame_get_hor_stride(info.frm);
            af_info->ver_stride = mpp_frame_get_ver_stride(info.frm);

            if(info.grp == NULL)
            {
               mpp_buffer_group_get_external(&info.grp,MPP_BUFFER_TYPE_EXT_DMA);
               
            }else{
                
                mpp_buffer_group_clear(info.grp);//清空旧组，因为旧的大小可能不够了，尺寸变了

                //clear只管解除映射，close掉这些fd需要我们自己来
                size_t fd_number = this->cma_buf_fd_list.size();
                for(int i=0 ; i < fd_number ; i++)
                {
                    close(this->cma_buf_fd_list[i]);
                }
                this->cma_buf_fd_list.clear();
            }

            for(int i=0 ; i < 24 ; i++)
            {
                int cma_fd = dma_buf_alloc(heap_fd,af_info->data_size);
                if(cma_fd < 0)
                {
                    printf("【vpu】{get_data_from_vpu_model_3 (dma_buf_alloc fail)} error ret=%d\n", ret);
                    mpp_frame_deinit(&info.frm);
                    return -1;
                }
                this->cma_buf_fd_list.push_back(cma_fd);



                MppBufferInfo mp_bf_info;
                memset(&mp_bf_info, 0, sizeof(mp_bf_info));

                mp_bf_info.fd = cma_fd;
                mp_bf_info.size = af_info->data_size;
                //mp_bf_info.type =  MPP_BUFFER_TYPE_EXT_DMA;
                mp_bf_info.type =  MPP_BUFFER_TYPE_EXT_DMA;

                ret = mpp_buffer_commit(info.grp, &mp_bf_info);
                if(ret != MPP_OK)
                {
                    printf("【vpu】{get_data_from_vpu_model_3 (mpp_buffer_commit '%d' fail)} error ret=%d\n",i, ret);
                    mpp_frame_deinit(&info.frm);
                    return -1;
                }
            }

            info.api->control(info.ctx, MPP_DEC_SET_EXT_BUF_GROUP, info.grp);

            info.api->control(info.ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
            if (ret != MPP_OK) {
                printf("【vpu】{get_data_from_vpu_model_3 (set info_change_ready fail)} error ret=%d\n", ret);
                mpp_frame_deinit(&info.frm);
                return -1;
            }

            mpp_frame_deinit(&info.frm);
            close(heap_fd);

            // 不 return，掉头取真帧
            return get_data_from_vpu_model_3(af_info, true);
        }
        
        //data帧
        
        //不会让buffer的引用计数+1，用户拿到后要自己去mpp_buffer_inc_ref()，不然还没来得及玩就被还了
        af_info->mb = mpp_frame_get_buffer(info.frm);//我们拿到的buffer计数器一开始就是1

        af_info->data_size = mpp_frame_get_buf_size(info.frm);
        af_info->hor_stride = mpp_frame_get_hor_stride(info.frm);
        af_info->ver_stride = mpp_frame_get_ver_stride(info.frm);
        af_info->fmt = mpp_frame_get_fmt(info.frm);
        af_info->data = mpp_buffer_get_ptr(af_info->mb);//
        af_info->buf_fd = mpp_buffer_get_fd(af_info->mb);
        af_info->pts = mpp_frame_get_pts(info.frm);
        af_info->frm = info.frm;//
  
        info.frm = NULL;
    
        return 1;

    }


    void result()
    {
        MPP_RET ret = mpp_frame_deinit(&this->info.frm);//问题3：还得太早了，用户拿到还没开始玩，就被还了
        if(ret != MPP_OK)
        {
            printf("【vpu】{result}:release_fail : %lld\n", m_pts_counter);
        }
        if(debug)
        {
            printf("【vpu】{result}:release_ok : %lld\n", m_pts_counter);
        }
        
    }

    void result(after_vpu_frame_info* af_info)
    {
        MPP_RET ret = mpp_frame_deinit(&af_info->frm);//问题3：还得太早了，用户拿到还没开始玩，就被还了
        if(ret != MPP_OK)
        {
            printf("【vpu】{result}:release_fail : %lld\n", m_pts_counter);
        }
        if(debug)
        {
            printf("【vpu】{result}:release_ok : %lld\n", m_pts_counter);
        }
    }


    ~PSQ_vpu()
    {
        
        mpp_buffer_group_put(info.grp); // 引用计数减一
        info.grp = NULL;
        mpp_destroy(info.ctx);

        //close掉所有fd
        size_t fd_number = this->cma_buf_fd_list.size();
        for(int i=0 ; i < fd_number ; i++)
        {
            close(this->cma_buf_fd_list[i]);
        }
        this->cma_buf_fd_list.clear();
    }

private:
    vpu_info info;
    signed long long int m_pts_counter = 0;
    std::vector<int> cma_buf_fd_list;



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
};



#endif