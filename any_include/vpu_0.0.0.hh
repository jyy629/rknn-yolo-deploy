#ifndef PSQ_VPU_HH
#define PSQ_VPU_HH

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

enum vpu_decode_type
{
    H265 = MPP_VIDEO_CodingHEVC,
    H264 = MPP_VIDEO_CodingAVC,
};

struct vpu_infp
{
    MppCtx   ctx = NULL;
    MppApi  *api = NULL;
    MppFrame frm = NULL;
};

class PSQ_vpu
{

public:

    PSQ_vpu(vpu_decode_type type)
    {
        printf("==========【vpu Mode 1: internal buffer】==========\n");
        MPP_RET ret = mpp_create(&info.ctx, &info.api);
        if (ret != MPP_OK) {
            printf("{构造函数}:: [mpp_create FAIL] ret=%d\n", ret);
            return;
        }

        MppDecCfg cfg = NULL;
        mpp_dec_cfg_init(&cfg);
        mpp_dec_cfg_set_s32(cfg, "base:split_parse", 0);
        ret = info.api->control(info.ctx, MPP_DEC_SET_CFG, cfg);
        mpp_dec_cfg_deinit(cfg);
        if (ret != MPP_OK) {
            printf("{构造函数}:: [SET_CFG FAIL] ret=%d\n", ret);
            mpp_destroy(info.ctx);
            return;
        }

        if (type == H265)
            ret = mpp_init(info.ctx, MPP_CTX_DEC, MPP_VIDEO_CodingHEVC);
        else
            ret = mpp_init(info.ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC);

        if (ret != MPP_OK) {
            printf("{构造函数}:: [mpp_init FAIL] ret=%d\n", ret);
            mpp_destroy(info.ctx);
            return;
        }

        printf("【vpu】init ok (type=%d)\n", type);
        printf("==========【vpu】初始化完成 ==========\n");
    }

    ~PSQ_vpu()
    {
        mpp_destroy(info.ctx);
    }

    int send_packet(void* data, size_t data_size)
    {
        if (data == NULL || data_size == 0) return -1;

        MppPacket pkt = NULL;
        mpp_packet_init(&pkt, data, data_size);
        mpp_packet_set_pos(pkt, data);
        mpp_packet_set_length(pkt, data_size);
        mpp_packet_set_pts(pkt, m_pts_counter++);

        MPP_RET ret = info.api->decode_put_packet(info.ctx, pkt);
        mpp_packet_deinit(&pkt);

        if (ret != MPP_OK)
        {
            printf("【vpu】{send_packet} FAIL ret=%d  pts=%lld\n", ret, m_pts_counter - 1);
            return ret;
        }
        printf("【vpu】{send_packet} OK  pts=%lld  size=%zu\n", m_pts_counter - 1, data_size);
        return 0;
    }

    int try_get_frame(void **out)
    {
        return _get_frame(out, false);
    }

    int try_get_frame_blocking(void **out)
    {
        return _get_frame(out, true);
    }

    void result()
    {
        if (info.frm)
        {
            mpp_frame_deinit(&info.frm);
            info.frm = NULL;
        }
    }

private:
    vpu_infp info;
    signed long long int m_pts_counter = 0;

    int _get_frame(void **out, bool blocking)
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
            return -1;
        }

        if (mpp_frame_get_info_change(info.frm) != 0)
        {
            printf("【vpu】{try_get_frame} info_change w=%u h=%u\n",
                   mpp_frame_get_width(info.frm),
                   mpp_frame_get_height(info.frm));

            info.api->control(info.ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
            mpp_frame_deinit(&info.frm);
            info.frm = NULL;
            return _get_frame(out, true);
        }

        MppBuffer mb = mpp_frame_get_buffer(info.frm);
        printf("【vpu】{try_get_frame} VIDEO FRAME  fd=%d  pts=%lld\n",
               mpp_buffer_get_fd(mb), mpp_frame_get_pts(info.frm));

        if (out && *out)
        {
            *out = mpp_buffer_get_ptr(mb);
        }
        return mpp_buffer_get_fd(mb);
    }
};

#endif