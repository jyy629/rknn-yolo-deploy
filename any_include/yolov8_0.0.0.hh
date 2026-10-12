/*
*@brief 专门处理yolov8模型后处理
*@warning 只能用于特定裁剪后的模型【双输出】
*@@details 
*由于yolov8的原模型单输出做量化后，sort模块输出经过sigmoid范围是（0-1），但是box输出范围为（0-800）左右。
*源模型末端有concat层合并2个张量为1个，但是zp和scale公用同一套导致sort模块精度严重坍缩
*此文件处理的模型为两种:A,B

*A: 裁剪了concat的同时，拔掉了box模块的dfl，在cpu上软处理。
*优势：1.源未裁剪模型softmax回退到cpu，然后拷贝npu输出到cpu，导致7350个全部要算一边，现在cpu软转可以先筛选sort输出，在软实现dfl，数量大大减小
*     2.源box量化参数scale过大（3.x）精度钝化，而裁剪到logits范围（+-16左右），尺度更细化
*
*B；只裁剪了concat模块，没有拔掉box模块的dfl


*@note 上层构建好类实例后，调用postprocessing()函数进行后处理
*       void draw_boxes（）函数按需选择
*/

#ifndef PSQ_YOLOV8_HH
#define PSQ_YOLOV8_HH

enum YoloModelType{
    YOLOV8_plane_B = 0,
    YOLOV8_plane_A = 1,
};

struct Plane_A_Limit{

    int min;//最小值
    int max;//最大值
    int width;//宽度
    int height;//高度
    int step;//步长
};

struct ClassRange {
    int class_id;
    int start;
    int end;
};

#include <stdio.h>
#include <rknpu2/rknn_api.h>
#include <vector>
#include <algorithm>
#include <pthread.h>
#include <cstdint>
#include <arm_neon.h>
#include <math.h>
#include <string>


// ===== 检测框 =====
struct YoloBox {
    float x1, y1, x2, y2;
    float conf;
    int class_id;
};

struct model_out_info{

    int32_t output_zp;//用于构建反量化表
    float output_scale;//用于构建反量化表
    int model_outshape[3];//[1, height, width]

    int8_t* output_str;

    float dequantiz_table[256];//用于反量化
    float dequantiz_sigmoid_table[256];//用于反量化sigmoid
    float dequantiz_exp[256];//用于反量化exp||只有box模块需要会被赋值（用于plane_A的dfl解码softmax）

    std::vector<Plane_A_Limit> plane_A_limit;//plane_A的中心点范围(只有box模块需要会被赋值)
};


struct YoloModelInfo{

    int search_eximate;//搜索框的估计值
    float iou_eximate;//iou阈值

    float scale;//坐标映射到原摄像头比例
    float pad_w;//宽度填充
    float pad_h;//高度填充
    float video_width;//视频宽度
    float video_height;//视频高度

    YoloModelType model_type;//模型类型
    model_out_info box_out_info_;//模型输出信息
    model_out_info sort_out_info_;//模型输出信息

};

class PSQ_YOLOv8
{

public:

    bool debug = false;


    PSQ_YOLOv8(
        rknn_tensor_attr* ex_input_attr,
        rknn_tensor_attr* ex_output_attr, 
        rknn_tensor_mem** ex_input_mem, 
        rknn_tensor_mem** ex_output_mem, 
        int ex_search_eximate,
        float ex_iou_eximate,
        float ex_video_width,
        float ex_video_height
    );
    
    void dump_info(const char* filepath, char mode, rknn_tensor_attr* input_attr, rknn_tensor_attr* output_attr);

    void postprocessing();

    void draw_boxes(void* img , size_t w, size_t h);

    void draw_boxes(void* img , size_t w, size_t h ,std::vector<YoloBox> boxes);

    ~PSQ_YOLOv8();

    
    std::vector <YoloBox>&& get_final_vec()
    {
        return std::move(final_vec_);
    }


private:
// ===== 模型信息 =====
    YoloModelInfo model_info_;//用于存储模型静态不会动了的信息
    std::vector <YoloBox>    search_out_vec_;
    std::vector <ClassRange> ranges_;
    std::vector <YoloBox>    final_vec_;


//===== 类函数 ======

    // ===== 反量化sigmoid =====
    void made_dequanziza_sigmoid();

    // ===== 搜索框B =====
    void search_box_B();

    // ===== 搜索框A =====
    void search_box_A();

    // ===== 前nms =====
    std::vector<uint8_t> before_nms();

    // ===== iou =====
    void iou_(std::vector<uint8_t>& keep_index);

};


#endif