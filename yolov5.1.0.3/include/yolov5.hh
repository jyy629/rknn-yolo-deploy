#ifndef PSQ_YOLOV5_HH
#define PSQ_YOLOV5_HH

enum examine_status {
    finaly_vector = 0,
    headout_vector = 1,
    dequantiza_list = 2,
};
#include <stdio.h>
#include <rknpu2/rknn_api.h>
#include <vector>
#include <pthread.h>
#include <cstdint>
#include <arm_neon.h>

// ===== 检测框 =====
struct YoloBox {
    float x, y, w, h;
    float conf;
    int class_id;
};

// ===== NMS 按类别分段 =====
struct ClassRange {
    int class_id;
    int start;
    int end;
};

// ===== 模型静态信息（由模型决定，构造后不变）=====
struct YoloModelInfo {
    int out_head_count = 3;
    int32_t output_zp = 0;
    float output_scale = 1.0f;
    int fmt = RKNN_TENSOR_NCHW;

    static constexpr int   IOU_MAX_DET = 512;
    static constexpr float aw[3][3] = {{10,16,33},{30,62,59},{116,156,373}};
    static constexpr float ah[3][3] = {{13,30,23},{61,45,119},{90,198,326}};
};

// ===== 每个输出头的线程参数 =====
struct HeadArg {
    int8_t* output;
    int8_t estimate;
    std::vector<YoloBox>* out_vec;
    int yolo_outshape[4];
    int fmt;
    int fact_xy_stride;
    float anchor_w[3];
    float anchor_h[3];

    pthread_mutex_t mutex_work;
    pthread_cond_t  cond_work;
    pthread_cond_t  cond_done;
    bool has_work;
    bool work_done;
    bool stop;

    class PSQ_YOLOv5* owner;
};

// ===== worker 线程入口 =====
void* yolov5_worker(void* arg);

// ===== YOLOv5 主类 =====
class PSQ_YOLOv5 {
public:
    friend void* yolov5_worker(void* arg);

    PSQ_YOLOv5(
        rknn_tensor_attr* input_attr,
        rknn_tensor_attr* output_attr,
        rknn_tensor_mem** input_mem,
        rknn_tensor_mem** output_mem,
        bool is_sigmoid  = true,
        int  estimate    = -70,
        float iou_thresh = 0.45f
    );

    ~PSQ_YOLOv5();

    void manage();
    void draw(uint8_t* user_buf, int width, int height);
    int  examine(examine_status status);

private:
    // 模型信息
    YoloModelInfo model_info_;
    float dequant_table_[256];

    // 运行时状态
    std::vector<HeadArg>              heads_;
    std::vector<std::vector<YoloBox>> head_out_vecs_;
    std::vector<YoloBox>              final_dets_;
    std::vector<ClassRange>           ranges_;
    std::vector<pthread_t>            thread_ids_;

    int   estimate_;
    float iou_thresh_;

    rknn_tensor_attr*  input_attr_;
    rknn_tensor_attr*  output_attr_;
    rknn_tensor_mem**  input_mem_;
    rknn_tensor_mem**  output_mem_;

    // 内部方法
    void build_dequant_table(bool is_sigmoid);
    void create_threads();
    void stop_threads();
    void dispatch_and_wait();
    void all_nms();

    void   decode_one_head(void* arg);
    float  out_fact_figure(float value, int row_or_line, int stride, int xy_or_wh, float anchor_hw);

    void nms_iou(uint32_t* keep_bits, std::vector<YoloBox>& all_dets);
    void to_soa(const YoloBox* base, int size,
                float* x1, float* y1, float* x2, float* y2, float* area);
    void nms_segment(uint32_t* keep, int base, int size,
                     const float* x1, const float* y1,
                     const float* x2, const float* y2,
                     const float* area, float thresh);
};

#endif