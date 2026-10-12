/*

1.创建PSQ_yolov5_rk类的实例
2.调用manage函数，管理线程的管理||在每次条用rknn_run+sync后，调用manage函数唤醒线程，进行下一次识别
3.调用draw_picture函数，绘制识别结果，传入PSQ_rga类的实例，绘制在rga的im_dma_va上

*/

#ifndef YLORKNN_HH
#define YLORKNN_HH

// 包含必要的头文件
#include <rknpu2/rknn_api.h>
#include <stdio.h>
#include <psq/check_error.hh>
#include <string.h>
#include <opencv2/opencv.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <pthread.h>
#include <vector>

#include "rga.hh"
//nms_include
#include <arm_neon.h>
#include <cstdint>
#include <cstring>
#include <algorithm>

class PSQ_yolov5_rk;
//相关结构体：

//用于存储每个线程的输出，识别后的结果【每个结构体代表一个框】
struct pthread_out {
    float x, y, w, h;//xy为中心点
    float conf;
    int class_id;
};

//用于存储每个类别的范围，用于nms
struct ClassRange {
        int class_id;
        int start;
        int end;
};


//用于存储每个线程的输入参数，打包进线程参数中
struct set_in_thread
{
    int8_t* output;
    int8_t estimate;// 置信度阈值
    std::vector<struct pthread_out>* pth_out_vec;
    int yolo_outshape[4];
    int fmt;
    int fact_xy_stride;
    float anchor_w[3];
    float anchor_h[3];
    pthread_mutex_t mutex_work;
    bool stop = false;//是否停止[线程停止标志位]

    PSQ_yolov5_rk* ex_calss;

};
void* pth_work(void* arg);

class PSQ_yolov5_rk
{
public:

    friend void* pth_work(void* arg);

    // ------------------------------------------------------------
    // 构造函数：1.构造反量化表
    // 参数：1.bool isornot_sigmoid【模型末端受否存在sigmoid，默认存在为1】
    // 参数：2.int modle_out_number【模型输出数，默认3】yolov5模型输出3个张量
    // 参数：3.rknn_tensor_attr【输入张量属性】
    // 参数：4.rknn_tensor_attr【输出张量属性】
    // 参数：5.rknn_tensor_mem【输入线程的内存映射结构体rknn_tensor_mem】
    // 参数：6.rknn_tensor_mem【输出线程的内存映射结构体rknn_tensor_mem】
    // ------------------------------------------------------------
PSQ_yolov5_rk(
    rknn_tensor_attr* inattr, rknn_tensor_attr* outattr,
    rknn_tensor_mem** inmem, rknn_tensor_mem** outmem,
    bool isornot_sigmoid = true,
    int modle_out_number = 3,
    int ex_estimate = -70,// 置信度阈值
    float ex_iou_thresh = 0.45f// iou阈值
    );

    void draw_picture(PSQ_rga& rga);

    void manage(bool ex_stop = false);

    int det_count() const { return (int)final_dets.size(); }

    ~PSQ_yolov5_rk();

private:

//内运作函数：--------------------------------------------------
    void all_nms();

    int pth_create();

    float out_fact_figure(float value, int row_or_line, int fact_xy_stride, int xy_ro_wh, float anchor_hw);

    void* search_yolo_output_neon(void* arg);

    uint32_t *iou_shiyan(uint32_t *keep_bits, std::vector<pthread_out> &all_dets);

    inline void to_soa(const pthread_out* base, int size, float* x1, float* y1, float* x2, float* y2, float* area);



    inline void nms_segment(uint32_t* keep, int base, int size,
                                const float* x1, const float* y1,
                                const float* x2, const float* y2,
                                const float* area, float thresh);

    void pth_dispatch_and_wait();

    // 退出：
    void pth_stop();
//------------------------------------------------------------------------------

    static const float aw[3][3];//anchor_w：预选框宽度
    static const float ah[3][3];//anchor_h：预选框高度
    float dequant_table[256];   //反量化表

    //limit_class(run):
    std::vector<set_in_thread> sets;//每个线程的输入参数
    std::vector<ClassRange> ranges;//每个类别的范围，用于nms
    std::vector<pthread_out> final_dets;//最终的检测结果【每个结构体代表一个框】
    std::vector<pthread_t> pth_ids;//每个线程的id
    std::vector<std::vector<pthread_out>> pth_out_vecs;
    int estimate;// 置信度阈值
    float iou_thresh;// iou阈值
    const int IOU_MAX_DET = 512;

    //limit_rknn:
    std::vector<int8_t*> output;//每个线程的输出地址，由零拷贝内存映射结构体rknn_tensor_mem的virt_addr字段提供
    rknn_tensor_attr* output_attr;//输出张量属性【结构体数组指针】
    rknn_tensor_attr* input_attr;//输入张量属性【结构体数组指针】
    rknn_tensor_mem* input_mem;//【结构体数组指针】输入线程的内存映射结构体rknn_tensor_mem
    rknn_tensor_mem* output_mem;//【结构体数组指针】输出线程的内存映射结构体rknn_tensor_mem
    int modle_out_number = 3;//模型输出数，默认3
};
#endif