#include "yolov8_0.0.0.hh"
#include <opencv2/opencv.hpp>
const std::vector<std::string> COCO80_CLASS_{
    "person",
    "bicycle",
    "car",
    "motorcycle",
    "airplane",
    "bus",
    "train",
    "truck",
    "boat",
    "traffic light",
    "fire hydrant",
    "stop sign",
    "parking meter",
    "bench",
    "bird",
    "cat",
    "dog",
    "horse",
    "sheep",
    "cow",
    "elephant",
    "bear",
    "zebra",
    "giraffe",
    "backpack",
    "umbrella",
    "handbag",
    "tie",
    "suitcase",
    "frisbee",
    "skis",
    "snowboard",
    "sports ball",
    "kite",
    "baseball bat",
    "baseball glove",
    "skateboard",
    "surfboard",
    "tennis racket",
    "bottle",
    "wine glass",
    "cup",
    "fork",
    "knife",
    "spoon",
    "bowl",
    "banana",
    "apple",
    "sandwich",
    "orange",
    "broccoli",
    "carrot",
    "hot dog",
    "pizza",
    "donut",
    "cake",
    "chair",
    "couch",
    "potted plant",
    "bed",
    "dining table",
    "toilet",
    "tv",
    "laptop",
    "mouse",
    "remote",
    "keyboard",
    "cell phone",
    "microwave",
    "oven",
    "toaster",
    "sink",
    "refrigerator",
    "book",
    "clock",
    "vase",
    "scissors",
    "teddy bear",
    "hair drier",
    "toothbrush"
};

PSQ_YOLOv8::PSQ_YOLOv8(
        rknn_tensor_attr* ex_input_attr,
        rknn_tensor_attr* ex_output_attr, 
        rknn_tensor_mem** ex_input_mem, 
        rknn_tensor_mem** ex_output_mem, 
        int ex_search_eximate,
        float ex_iou_eximate,
        float ex_video_width,
        float ex_video_height
    )
    //两种模型：plane_B：sort(1,80,7350)    box(1,4,7350)
    //         plane_A: sort(1,80,?)       box(1,64,?)  ?需要根据模型输入shape确定
    //         plane_A：我们把模型的box部分裁剪的比较深，需要我们手动软cpu做logits转xywh后处理
    //this->model_info_.output_zp = ex_output_attr->zp;//用于构建反量化表
    //this->model_info_.output_scale = ex_output_attr->scale;//用于构建反量化表
{
    this->model_info_.search_eximate = ex_search_eximate;
    this->model_info_.iou_eximate = ex_iou_eximate;
    
    if(ex_output_attr[0].dims[1] != 64 && ex_output_attr[1].dims[1] != 64)//说明是plane_B
    {   
        this->model_info_.model_type = YOLOV8_plane_B;
        if(ex_output_attr[0].dims[1] != 80)//说明[0]是box
        {
            for(int i = 0 ; i < 3; i++ )
            {
                this->model_info_.box_out_info_.model_outshape[i] = ex_output_attr[0].dims[i];
                this->model_info_.sort_out_info_.model_outshape[i] = ex_output_attr[1].dims[i];
            }
            this->model_info_.sort_out_info_.output_str = (int8_t*)ex_output_mem[1]->virt_addr;
            this->model_info_.box_out_info_.output_str = (int8_t*)ex_output_mem[0]->virt_addr;

            this->model_info_.box_out_info_.output_zp   = ex_output_attr[0].zp;
            this->model_info_.box_out_info_.output_scale = ex_output_attr[0].scale;

            this->model_info_.sort_out_info_.output_zp   = ex_output_attr[1].zp;
            this->model_info_.sort_out_info_.output_scale = ex_output_attr[1].scale;
        }else{//说明[0]是sort
            for(int i = 0 ; i < 3; i++ )
            {
                this->model_info_.box_out_info_.model_outshape[i] = ex_output_attr[1].dims[i];
                this->model_info_.sort_out_info_.model_outshape[i] = ex_output_attr[0].dims[i];
            }
            this->model_info_.sort_out_info_.output_str = (int8_t*)ex_output_mem[0]->virt_addr;
            this->model_info_.box_out_info_.output_str = (int8_t*)ex_output_mem[1]->virt_addr;

            this->model_info_.box_out_info_.output_zp   = ex_output_attr[1].zp;
            this->model_info_.box_out_info_.output_scale = ex_output_attr[1].scale;

            this->model_info_.sort_out_info_.output_zp   = ex_output_attr[0].zp;
            this->model_info_.sort_out_info_.output_scale = ex_output_attr[0].scale;
        }
    }else{//说明是plane_A

        this->model_info_.model_type = YOLOV8_plane_A;
        if(ex_output_attr[0].dims[1] !=80){//说明[0]是box

            for(int i = 0 ; i < 3; i++ )
            {
                this->model_info_.box_out_info_.model_outshape[i] = ex_output_attr[0].dims[i];
                this->model_info_.sort_out_info_.model_outshape[i] = ex_output_attr[1].dims[i];
            }
            this->model_info_.box_out_info_.output_str = (int8_t*)ex_output_mem[0]->virt_addr;
            this->model_info_.sort_out_info_.output_str = (int8_t*)ex_output_mem[1]->virt_addr;

            this->model_info_.box_out_info_.output_zp   = ex_output_attr[0].zp;
            this->model_info_.box_out_info_.output_scale = ex_output_attr[0].scale;

            this->model_info_.sort_out_info_.output_zp   = ex_output_attr[1].zp;
            this->model_info_.sort_out_info_.output_scale = ex_output_attr[1].scale;

        }else{//说明[0]是sort
            for(int i = 0 ; i < 3; i++ )
            {
                this->model_info_.sort_out_info_.model_outshape[i] = ex_output_attr[0].dims[i];
                this->model_info_.box_out_info_.model_outshape[i] = ex_output_attr[1].dims[i];
            }
            this->model_info_.sort_out_info_.output_str = (int8_t*)ex_output_mem[0]->virt_addr;
            this->model_info_.box_out_info_.output_str = (int8_t*)ex_output_mem[1]->virt_addr;

            this->model_info_.box_out_info_.output_zp   = ex_output_attr[1].zp;
            this->model_info_.box_out_info_.output_scale = ex_output_attr[1].scale;

            this->model_info_.sort_out_info_.output_zp   = ex_output_attr[0].zp;
            this->model_info_.sort_out_info_.output_scale = ex_output_attr[0].scale;
        }
    }
    
    //为plane_A和计算中心点的步长和范围
    this->model_info_.box_out_info_.plane_A_limit.resize(3);
    int num = 0;
    for(int i = 0 ; i < 3 ; i++)
    {
        int min_;//最小值
        int max_;//最大值
        int width_;//宽度
        int height_;//高度
        int step_;//步长

        step_   = 8 << i;  //8,16,32
        width_  = ex_input_attr[0].dims[2]/step_;//(nhwc)W
        height_ = ex_input_attr[0].dims[1]/step_;//(nhwc)H

        min_ = num;
        max_ = min_ + width_ * height_ -1 ;
        num = max_ + 1;
        
        this->model_info_.box_out_info_.plane_A_limit[i].step = step_;
        this->model_info_.box_out_info_.plane_A_limit[i].min = min_;
        this->model_info_.box_out_info_.plane_A_limit[i].max = max_;
        this->model_info_.box_out_info_.plane_A_limit[i].width = width_;
        this->model_info_.box_out_info_.plane_A_limit[i].height = height_;
    }


    this->model_info_.scale = std::min(ex_input_attr->dims[2]/ex_video_width, ex_input_attr->dims[1]/ex_video_height);
    float model_in_w = ex_input_attr->dims[2];
    float model_in_h = ex_input_attr->dims[1];
    this->model_info_.pad_h = (model_in_h - (ex_video_height * this->model_info_.scale)) / 2.0f;
    this->model_info_.pad_w = (model_in_w - (ex_video_width * this->model_info_.scale)) / 2.0f;

    this->model_info_.video_width = ex_video_width;
    this->model_info_.video_height = ex_video_height;

    this->search_out_vec_.reserve(1024);//预分配内存，避免动态分配内存


    made_dequanziza_sigmoid();


}

PSQ_YOLOv8::~PSQ_YOLOv8()
{

}


void PSQ_YOLOv8::made_dequanziza_sigmoid()
    {
        int32_t sort_zp    = this->model_info_.sort_out_info_.output_zp;
        float   sort_scale = this->model_info_.sort_out_info_.output_scale;

        int32_t box_zp    = this->model_info_.box_out_info_.output_zp;
        float   box_scale = this->model_info_.box_out_info_.output_scale;

        for (int i = 0; i < 256; i++) {
            int8_t raw   = (int8_t)(i - 128);
            float  logit = (raw - sort_zp) * sort_scale;

            this->model_info_.sort_out_info_.dequantiz_table[i] = logit;

            this->model_info_.sort_out_info_.dequantiz_sigmoid_table[i] = 1.0f / (1.0f + expf(-logit));
        }

        for (int i = 0; i < 256; i++) {
            int8_t raw   = (int8_t)(i - 128);
            float  logit = (raw - box_zp) * box_scale;

            this->model_info_.box_out_info_.dequantiz_table[i] = logit;

            this->model_info_.box_out_info_.dequantiz_sigmoid_table[i] = 1.0f / (1.0f + expf(-logit));

            this->model_info_.box_out_info_.dequantiz_exp[i] = expf(logit);
        }

}


void PSQ_YOLOv8::search_box_B()
{
    search_out_vec_.clear();
    int8_t* sort_output = this->model_info_.sort_out_info_.output_str;
    int8_t* box_output = this->model_info_.box_out_info_.output_str;
    int sort_H , sort_W , box_H , box_W;
    int eximate = model_info_.search_eximate;
    sort_H = this->model_info_.sort_out_info_.model_outshape[1];//行
    sort_W = this->model_info_.sort_out_info_.model_outshape[2];//列
    box_H  = this->model_info_.box_out_info_.model_outshape[1];
    box_W  = this->model_info_.box_out_info_.model_outshape[2];
    int sort_sum = sort_H;//多少个类别

    int8_t* class_base = sort_output;
    int8_t* y_base = box_output+box_W;//y在第2行
    int8_t* w_base = box_output+box_W*2;//w在第3行
    int8_t* h_base = box_output+box_W*3;//h在第4行

    //匿名函数用于根据搜索索引来给容器赋值
    auto process = [&](int c , int class_id)
    {

        float conf = this->model_info_.sort_out_info_.dequantiz_sigmoid_table[class_base[class_id*sort_W+c] + 128];
        float cx,cy,w,h;
        cx = this->model_info_.box_out_info_.dequantiz_table[box_output[c] + 128];
        cy = this->model_info_.box_out_info_.dequantiz_table[y_base    [c] + 128];
        w = this->model_info_.box_out_info_.dequantiz_table [w_base    [c] + 128];
        h = this->model_info_.box_out_info_.dequantiz_table [h_base    [c] + 128];
        float x1 = cx - w * 0.5f;   // 左上角 x
        float y1 = cy - h * 0.5f;   // 左上角 y
        float x2 = cx + w * 0.5f;   // 右下角 x
        float y2 = cy + h * 0.5f;   // 右下角 y

        float rx1 = (x1 - model_info_.pad_w) / model_info_.scale;
        float ry1 = (y1 - model_info_.pad_h) / model_info_.scale;
        float rx2 = (x2 - model_info_.pad_w) / model_info_.scale;
        float ry2 = (y2 - model_info_.pad_h) / model_info_.scale;
        rx1 = std::max(rx1, 0.0f);
        ry1 = std::max(ry1, 0.0f);
        rx2 = std::min(rx2, model_info_.video_width);
        ry2 = std::min(ry2, model_info_.video_height);

        struct YoloBox box = {rx1, ry1, rx2, ry2, conf, class_id};
        search_out_vec_.push_back(box);


    };

    const int8x16_t vth = vdupq_n_s8(eximate);

    for(int class_id = 0; class_id < sort_sum; class_id++)
    {
        int8_t* now_class_base = class_base + class_id * sort_W;
        
        int c = 0;
        for (; c + 16 <= sort_W; c += 16){

            uint8x16_t hit_mask = vcgeq_s8(vld1q_s8(now_class_base+c), vth); //比较是否大于等于阈值
            uint64_t m0 = vgetq_lane_u64(vreinterpretq_u64_u8(hit_mask), 0);//获取前64bit
            uint64_t m1 = vgetq_lane_u64(vreinterpretq_u64_u8(hit_mask), 1);//获取后64bit

            uint64_t fron_hit = m0 & 0x8080808080808080ULL;//从单个字节8bit的11111111->10000000
            uint64_t back_hit = m1 & 0x8080808080808080ULL;//从单个字节8bit的11111111->00000001

            while(fron_hit)
            {
                int k = __builtin_ctzll(fron_hit) >> 3;//获取第一个1的位数 除以8得到索引
                process(k+c , class_id);
                fron_hit &=fron_hit-1;//把原本100000000变为011111111然后再自己按位与，以此消除
            }

            while(back_hit)
            {
                int k = __builtin_ctzll(back_hit) >> 3;//获取第一个1的位数 除以8得到索引
                process(k+c+8 , class_id);
                back_hit &=back_hit-1;//把原本100000000变为011111111然后再自己按位与，以此消除
            }

        }
        for(; c < sort_W ; c++)
        {
            if(now_class_base[c] >= eximate)process(c , class_id);

        }
    }
}


void PSQ_YOLOv8::search_box_A()
{
    search_out_vec_.clear();
    int8_t* sort_output = this->model_info_.sort_out_info_.output_str;
    int8_t* box_output = this->model_info_.box_out_info_.output_str;
    int sort_H , sort_W , box_H , box_W;
    int eximate = model_info_.search_eximate;
    sort_H = this->model_info_.sort_out_info_.model_outshape[1];//行
    sort_W = this->model_info_.sort_out_info_.model_outshape[2];//列
    box_H  = this->model_info_.box_out_info_.model_outshape[1];
    box_W  = this->model_info_.box_out_info_.model_outshape[2];
    int sort_sum = sort_H;//多少个类别

    int8_t* class_base = sort_output;
    int8_t* y_base = box_output+box_W;//y在第2行
    int8_t* w_base = box_output+box_W*2;//w在第3行
    int8_t* h_base = box_output+box_W*3;//h在第4行

    //匿名函数用于根据搜索索引来给容器赋值
    auto process = [&](int c , int class_id)
    {   

        //以下为整套dfl解码过程
        float left_[16];
        int8_t* left_base = box_output+c;
        float left_denor = 0.0f;//分母
        float left_math_expect = 0.0f;//数学期望值

        float top_[16];
        int8_t* top_base = box_output+box_W*16+c;
        float top_denor = 0.0f;//分母
        float top_math_expect = 0.0f;//数学期望值

        float right_[16];
        int8_t* right_base = box_output+box_W*32+c;
        float right_denor = 0.0f;//分母
        float right_math_expect = 0.0f;//数学期望值

        float bottom_[16];
        int8_t* bottom_base = box_output+box_W*48+c;
        float bottom_denor = 0.0f;//分母
        float bottom_math_expect = 0.0f;//数学期望值

        float gx , gy , stride;
        if(c >= this->model_info_.box_out_info_.plane_A_limit[0].min && c <= this->model_info_.box_out_info_.plane_A_limit[0].max )
        {
            int local_c = c - this->model_info_.box_out_info_.plane_A_limit[0].min;
            stride = this->model_info_.box_out_info_.plane_A_limit[0].step;//8
            gx = local_c % this->model_info_.box_out_info_.plane_A_limit[0].width;
            gy = local_c / this->model_info_.box_out_info_.plane_A_limit[0].width;
        }
        else if(c >= this->model_info_.box_out_info_.plane_A_limit[1].min && c <= this->model_info_.box_out_info_.plane_A_limit[1].max)
        {
            int local_c = c - this->model_info_.box_out_info_.plane_A_limit[1].min;
            stride = this->model_info_.box_out_info_.plane_A_limit[1].step;//16
            gx = local_c % this->model_info_.box_out_info_.plane_A_limit[1].width;
            gy = local_c / this->model_info_.box_out_info_.plane_A_limit[1].width;
        }
        else if(c >= this->model_info_.box_out_info_.plane_A_limit[2].min && c <= this->model_info_.box_out_info_.plane_A_limit[2].max)
        {
            int local_c = c - this->model_info_.box_out_info_.plane_A_limit[2].min;
            stride = this->model_info_.box_out_info_.plane_A_limit[2].step;//32
            gx = local_c % this->model_info_.box_out_info_.plane_A_limit[2].width;
            gy = local_c / this->model_info_.box_out_info_.plane_A_limit[2].width;
        }

        //赋值并计算分母
        for(int i = 0 ; i < 16 ; i++)
        {
            left_[i]   = this->model_info_.box_out_info_.dequantiz_exp[left_base  [i * box_W] + 128];
            top_[i]    = this->model_info_.box_out_info_.dequantiz_exp[top_base   [i * box_W] + 128];
            right_[i]  = this->model_info_.box_out_info_.dequantiz_exp[right_base [i * box_W] + 128];
            bottom_[i] = this->model_info_.box_out_info_.dequantiz_exp[bottom_base[i * box_W] + 128];
            left_denor += left_[i];
            top_denor += top_[i];
            right_denor += right_[i];
            bottom_denor += bottom_[i];
        }

        //归一化
        for(int i = 0 ; i < 16 ; i++)
        {
            left_[i] /= left_denor;
            top_[i] /= top_denor;
            right_[i] /= right_denor;
            bottom_[i] /= bottom_denor;
        }

        //计算数学期望值
        for(int i = 0 ; i < 16 ; i++)
        {
            left_math_expect += i*left_[i];
            top_math_expect += i*top_[i];
            right_math_expect += i*right_[i];
            bottom_math_expect += i*bottom_[i];
        }

        float x1 , x2 , y1 , y2;
        x1 = (gx + 0.5 - left_math_expect  ) * stride;
        y1 = (gy + 0.5 - top_math_expect   ) * stride;
        x2 = (gx + 0.5 + right_math_expect ) * stride;
        y2 = (gy + 0.5 + bottom_math_expect) * stride;

        //dfl解码过程结束，接下来坐标映射到源视频尺度
        float conf = this->model_info_.sort_out_info_.dequantiz_sigmoid_table[class_base[class_id*sort_W+c] + 128];
        
        float rx1 = (x1 - model_info_.pad_w) / model_info_.scale;
        float ry1 = (y1 - model_info_.pad_h) / model_info_.scale;
        float rx2 = (x2 - model_info_.pad_w) / model_info_.scale;
        float ry2 = (y2 - model_info_.pad_h) / model_info_.scale;
        rx1 = std::max(rx1, 0.0f);
        ry1 = std::max(ry1, 0.0f);
        rx2 = std::min(rx2, model_info_.video_width);
        ry2 = std::min(ry2, model_info_.video_height);

        //映射结束，放数据
        struct YoloBox box = {rx1, ry1, rx2, ry2, conf, class_id};
        search_out_vec_.push_back(box);


    };

    const int8x16_t vth = vdupq_n_s8(eximate);

    for(int class_id = 0; class_id < sort_sum; class_id++)
    {
        int8_t* now_class_base = class_base + class_id * sort_W;
        
        int c = 0;
        for (; c + 16 <= sort_W; c += 16){

            uint8x16_t hit_mask = vcgeq_s8(vld1q_s8(now_class_base+c), vth); //比较是否大于等于阈值
            uint64_t m0 = vgetq_lane_u64(vreinterpretq_u64_u8(hit_mask), 0);//获取前64bit
            uint64_t m1 = vgetq_lane_u64(vreinterpretq_u64_u8(hit_mask), 1);//获取后64bit

            uint64_t fron_hit = m0 & 0x8080808080808080ULL;//从单个字节8bit的11111111->10000000
            uint64_t back_hit = m1 & 0x8080808080808080ULL;//从单个字节8bit的11111111->00000001

            while(fron_hit)
            {
                int k = __builtin_ctzll(fron_hit) >> 3;//获取第一个1的位数 除以8得到索引
                process(k+c , class_id);
                fron_hit &=fron_hit-1;//把原本100000000变为011111111然后再自己按位与，以此消除
            }

            while(back_hit)
            {
                int k = __builtin_ctzll(back_hit) >> 3;//获取第一个1的位数 除以8得到索引
                process(k+c+8 , class_id);
                back_hit &=back_hit-1;//把原本100000000变为011111111然后再自己按位与，以此消除
            }

        }
        for(; c < sort_W ; c++)
        {
            if(now_class_base[c] >= eximate)process(c , class_id);

        }
    }
}


std::vector<uint8_t> PSQ_YOLOv8::before_nms()
{
    ranges_.clear();
    final_vec_.clear();

    if (search_out_vec_.empty())
        return {};

    std::sort(search_out_vec_.begin(), search_out_vec_.end(),
    [](const YoloBox& a, const YoloBox& b) {
        if (a.class_id != b.class_id)
            return a.class_id < b.class_id;
        return a.conf > b.conf;
    });

    for (size_t i = 0; i < search_out_vec_.size(); ) {
        int cid = search_out_vec_[i].class_id;
        size_t j = i;
        while (j < search_out_vec_.size() && search_out_vec_[j].class_id == cid) ++j;
        ranges_.push_back({cid, (int)i, (int)j - 1});
        i = j;
    }

    std::vector<uint8_t> keep_index(search_out_vec_.size(), 1);
    return keep_index;
}


void PSQ_YOLOv8::iou_(std::vector<uint8_t>& keep_index)
{
    size_t n = search_out_vec_.size();
    std::vector<float> area(n);

    for (size_t idx = 0; idx < n; idx++) {
        area[idx] = (search_out_vec_[idx].x2 - search_out_vec_[idx].x1)
                    * (search_out_vec_[idx].y2 - search_out_vec_[idx].y1);
    }

    for (const ClassRange& range : ranges_) {
        int size = range.end - range.start + 1;
        if (size < 2) continue;

        int start = range.start;
        int end   = range.end;

        for (int i = start; i <= end; i++) {
            if (!keep_index[i]) continue;

            for (int j = i + 1; j <= end; j++) {
                if (!keep_index[j]) continue;

                float inter_x1 = std::max(search_out_vec_[i].x1, search_out_vec_[j].x1);
                float inter_y1 = std::max(search_out_vec_[i].y1, search_out_vec_[j].y1);
                float inter_x2 = std::min(search_out_vec_[i].x2, search_out_vec_[j].x2);
                float inter_y2 = std::min(search_out_vec_[i].y2, search_out_vec_[j].y2);
                float inter_w  = std::max(0.0f, inter_x2 - inter_x1);
                float inter_h  = std::max(0.0f, inter_y2 - inter_y1);
                float inter_area = inter_w * inter_h;

                float union_area = area[i] + area[j] - inter_area;
                if (union_area <= 0.0f) continue;   //除零保护
                float iou = inter_area / union_area;
                
                if (iou > model_info_.iou_eximate) {
                    keep_index[j] = 0;
                }
            }
        }
    }

    for (size_t idx = 0; idx < keep_index.size(); idx++) {
        if (keep_index[idx]) {
            final_vec_.push_back(search_out_vec_[idx]);
        }
    }
}


void PSQ_YOLOv8::dump_info(const char* filepath, char mode, rknn_tensor_attr* input_attr, rknn_tensor_attr* output_attr)
{
    FILE* fp = fopen(filepath, "w");
    if (!fp) { printf("ERROR: 无法打开 %s\n", filepath); return; }

    fprintf(fp, "========================================================\n");
    fprintf(fp, "           YOLOv8 Plane %c 验证报告\n", mode);
    fprintf(fp, "========================================================\n\n");

    // ===== 零、Tensor 信息 =====
    fprintf(fp, "========== 零、Tensor 信息 ==========\n");
    fprintf(fp, "--- 输入 Tensor ---\n");
    fprintf(fp, "  dims  = [%d, %d, %d, %d]\n",
            input_attr->dims[0], input_attr->dims[1], input_attr->dims[2], input_attr->dims[3]);
    fprintf(fp, "  fmt   = %d\n", input_attr->fmt);
    fprintf(fp, "  type  = %d\n", input_attr->type);
    fprintf(fp, "  size  = %d\n", input_attr->size);

    for (int i = 0; i < 2; i++) {
        fprintf(fp, "\n--- 输出 Tensor [%d] ---\n", i);
        fprintf(fp, "  dims  = [%d, %d, %d, %d]\n",
                output_attr[i].dims[0], output_attr[i].dims[1],
                output_attr[i].dims[2], output_attr[i].dims[3]);
        fprintf(fp, "  fmt   = %d\n", output_attr[i].fmt);
        fprintf(fp, "  type  = %d\n", output_attr[i].type);
        fprintf(fp, "  size  = %d\n", output_attr[i].size);
        fprintf(fp, "  zp    = %d\n", output_attr[i].zp);
        fprintf(fp, "  scale = %f\n", output_attr[i].scale);
    }

    // ===== 一、Sort 反量化表 =====
    fprintf(fp, "\n========== 一、Sort 反量化表 ==========\n");
    fprintf(fp, "zp = %d, scale = %f\n",
            model_info_.sort_out_info_.output_zp,
            model_info_.sort_out_info_.output_scale);
    fprintf(fp, "公式: logit = (raw - zp) * scale; sigmoid = 1/(1+exp(-logit))\n\n");
    fprintf(fp, "  int8  | uint8 |    logit     | sigmoid\n");
    fprintf(fp, "  ------+-------+--------------+---------\n");
    for (int i = 0; i < 256; i++) {
        int8_t raw = (int8_t)(i - 128);
        fprintf(fp, "  %4d  |  %3d  | %12.6f | %.6f\n",
                raw, i,
                model_info_.sort_out_info_.dequantiz_table[i],
                model_info_.sort_out_info_.dequantiz_sigmoid_table[i]);
    }

    // ===== 二、Sort 真实输出直方图 =====
    fprintf(fp, "\n========== 二、Sort 真实输出直方图 ==========\n");
    {
        int8_t* sort_out = model_info_.sort_out_info_.output_str;
        int sort_H = model_info_.sort_out_info_.model_outshape[1];
        int sort_W = model_info_.sort_out_info_.model_outshape[2];
        int total = sort_H * sort_W;

        fprintf(fp, "sort shape: [1, %d, %d], 共 %d 个值\n\n", sort_H, sort_W, total);

        int hist[256] = {0};
        for (int i = 0; i < total; i++) {
            hist[(uint8_t)sort_out[i]]++;
        }

        fprintf(fp, "  int8  | uint8 |   count   |  %%        | sigmoid\n");
        fprintf(fp, "  ------+-------+-----------+-----------+---------\n");
        int nonzero = 0;
        for (int i = 0; i < 256; i++) {
            if (hist[i] > 0) {
                nonzero++;
                int8_t raw = (int8_t)(i - 128);
                fprintf(fp, "  %4d  |  %3d  | %9d | %6.2f%%   | %f\n",
                        raw, i, hist[i],
                        100.0f * hist[i] / total,
                        model_info_.sort_out_info_.dequantiz_sigmoid_table[i]);
            }
        }
        fprintf(fp, "  (非零条目: %d / 256)\n", nonzero);

        // 逐类统计 > zp 的格子数
        int sort_zp = model_info_.sort_out_info_.output_zp;
        fprintf(fp, "\n--- 逐类统计：值 > zp(%d) 的格子数 ---\n", sort_zp);
        fprintf(fp, "  class_id |  >zp 格子数 |  =zp 格子数 |  max\n");
        fprintf(fp, "  ---------+-------------+-------------+------\n");
        int any_above = 0;
        for (int cls = 0; cls < sort_H; cls++) {
            int8_t* row = sort_out + cls * sort_W;
            int above = 0, at_zp = 0;
            int8_t max_val = -128;
            for (int c = 0; c < sort_W; c++) {
                if (row[c] > sort_zp) above++;
                else if (row[c] == sort_zp) at_zp++;
                if (row[c] > max_val) max_val = row[c];
            }
            if (above > 0) any_above++;
            fprintf(fp, "  %8d | %11d | %11d | %5d\n", cls, above, at_zp, max_val);
        }
        fprintf(fp, "  (有 >zp 格子的类别数: %d / %d)\n", any_above, sort_H);

        // 逐格 argmax
        fprintf(fp, "\n--- 逐格 argmax (共 %d 格) ---\n", sort_W);
        int argmax_hist[256] = {0};
        int cells_above = 0;
        for (int c = 0; c < sort_W; c++) {
            int8_t best_val = -128;
            for (int cls = 0; cls < sort_H; cls++) {
                int8_t val = sort_out[cls * sort_W + c];
                if (val > best_val) best_val = val;
            }
            argmax_hist[(uint8_t)best_val]++;
            if (best_val > sort_zp) cells_above++;
        }
        fprintf(fp, "  argmax 值分布:\n");
        fprintf(fp, "  int8  | uint8 | 格子数  |  %%\n");
        fprintf(fp, "  ------+-------+---------+-------\n");
        for (int i = 0; i < 256; i++) {
            if (argmax_hist[i] > 0) {
                int8_t raw = (int8_t)(i - 128);
                fprintf(fp, "  %4d  |  %3d  | %7d | %5.2f%%\n",
                        raw, i, argmax_hist[i],
                        100.0f * argmax_hist[i] / sort_W);
            }
        }
        fprintf(fp, "  (argmax > zp 的格子数: %d / %d)\n", cells_above, sort_W);
    }

    // ===== 二点五、Box 输出反量化统计 =====
    fprintf(fp, "\n========== 二点五、Box 输出反量化统计 ==========\n");
    {
        int8_t* box_out = model_info_.box_out_info_.output_str;
        int box_H  = model_info_.box_out_info_.model_outshape[1];
        int box_W  = model_info_.box_out_info_.model_outshape[2];
        int box_zp = model_info_.box_out_info_.output_zp;
        float box_scale = model_info_.box_out_info_.output_scale;
        int box_total = box_H * box_W;

        fprintf(fp, "box shape: [1, %d, %d], 共 %d 个值\n", box_H, box_W, box_total);
        fprintf(fp, "box zp = %d, scale = %f\n", box_zp, box_scale);
        fprintf(fp, "反量化公式: logit = (int8 - %d) * %f\n", box_zp, box_scale);
        fprintf(fp, "理论范围: [%f, %f]\n",
                (-128.0f - box_zp) * box_scale,
                (127.0f - box_zp) * box_scale);

        float box_min = 1e9f, box_max = -1e9f;
        double box_sum = 0.0;
        for (int i = 0; i < box_total; i++) {
            float val = (box_out[i] - box_zp) * box_scale;
            if (val < box_min) box_min = val;
            if (val > box_max) box_max = val;
            box_sum += val;
        }
        float box_mean = box_sum / box_total;

        fprintf(fp, "实际范围: min=%f, max=%f, mean=%f\n", box_min, box_max, box_mean);
        fprintf(fp, "exp(min)=%e, exp(max)=%e\n", expf(box_min), expf(box_max));
        fprintf(fp, "DFL softmax 结论: %s\n",
                (box_max - box_min < 20.0f) ?
                "范围<20, exp_table[256] 可直接用, 无需减max" :
                "范围较大, 建议先减max再查表");
    }

    // ===== 三、search_box 结果 =====
    fprintf(fp, "\n========== 三、search_box 输出 ==========\n");
    fprintf(fp, "检测框总数: %zu\n", search_out_vec_.size());
    if (search_out_vec_.empty()) {
        fprintf(fp, "\n  (无检测框)\n");
    } else {
        for (size_t i = 0; i < search_out_vec_.size(); i++) {
            auto& b = search_out_vec_[i];
            fprintf(fp, "  [%3zu] class=%2d  conf=%.4f  (%.1f,%.1f)-(%.1f,%.1f)\n",
                    i, b.class_id, b.conf, b.x1, b.y1, b.x2, b.y2);
        }
    }

    // ===== 三点五、搜索命中统计 =====
    {
        int8_t* sort_out = model_info_.sort_out_info_.output_str;
        int sort_H = model_info_.sort_out_info_.model_outshape[1];
        int sort_W = model_info_.sort_out_info_.model_outshape[2];
        int threshold = model_info_.search_eximate;

        fprintf(fp, "\n========== 三点五、搜索命中统计 ==========\n");
        fprintf(fp, "阈值(int8): %d\n", threshold);

        int hit_cells = 0;        // 至少命中1个类的anchor数
        int total_hits = 0;       // 总命中数
        int max_classes = 0;      // 单anchor最多命中类数
        int hist_classes[81] = {0}; // 命中1~80个类的anchor数分布

        for (int c = 0; c < sort_W; c++) {
            int hit_count = 0;
            for (int cls = 0; cls < sort_H; cls++) {
                if (sort_out[cls * sort_W + c] >= threshold) hit_count++;
            }
            if (hit_count > 0) {
                hit_cells++;
                total_hits += hit_count;
                if (hit_count > max_classes) max_classes = hit_count;
                if (hit_count <= 80) hist_classes[hit_count]++;
            }
        }

        fprintf(fp, "总 anchor 数: %d\n", sort_W);
        fprintf(fp, "命中 anchor 数 (>=%d): %d (%.2f%%)\n",
                threshold, hit_cells, 100.0f * hit_cells / sort_W);
        fprintf(fp, "search_out_vec_ 大小: %zu\n", search_out_vec_.size());
        fprintf(fp, "平均每命中anchor的类数: %.2f\n",
                hit_cells > 0 ? (float)total_hits / hit_cells : 0.0f);
        fprintf(fp, "单anchor最多命中类数: %d\n", max_classes);

        fprintf(fp, "\n每anchor命中类数分布:\n");
        fprintf(fp, "  命中类数 | anchor数 |  %%\n");
        fprintf(fp, "  ---------+----------+------\n");
        for (int i = 1; i <= max_classes && i <= 80; i++) {
            if (hist_classes[i] > 0) {
                fprintf(fp, "  %8d | %8d | %5.2f%%\n",
                        i, hist_classes[i], 100.0f * hist_classes[i] / hit_cells);
            }
        }
        fprintf(fp, "\nNMS建议: %s\n",
                (hit_cells < 500) ?
                "命中<500, 普通NMS随便写" :
                (max_classes <= 3) ?
                "命中多但每anchor类数少, 可先做argmax单标签筛选" :
                "命中多且每anchor多类, 需要优化NMS");
    }

    fprintf(fp, "\n========================================================\n");
    fprintf(fp, "                    报告结束\n");
    fprintf(fp, "========================================================\n");
    fclose(fp);
    printf("验证报告已写入: %s\n", filepath);
}

void PSQ_YOLOv8::postprocessing()
{
    if(this->model_info_.model_type == YOLOV8_plane_A)
    {
        search_box_A();
    }
    else if(this->model_info_.model_type == YOLOV8_plane_B)
    {
        search_box_B();
    }

    auto keep = before_nms();
    iou_(keep);
    if(debug)
    {
        printf("【YOLOv8】postprocessing: final_vec_ 大小: %zu\n", final_vec_.size());
    }
    
}


void PSQ_YOLOv8::draw_boxes(void* img , size_t w, size_t h)
{   

    if(final_vec_.empty())
    {   if(debug)
        {
            printf("【YOLOv8】draw_boxes: 无检测框\n");
        }
        return;
    }

    cv::Mat frame((int)h, (int)w, CV_8UC3, img);

    for (const auto& box : final_vec_)
    {
        int x1 = std::max(0, (int)box.x1);
        int y1 = std::max(0, (int)box.y1);
        int x2 = std::min((int)w - 1, (int)box.x2);
        int y2 = std::min((int)h - 1, (int)box.y2);

        cv::Scalar red(0, 0, 255);
        cv::Scalar white(255, 255, 255);

        // 红色细框
        cv::rectangle(frame, cv::Point(x1, y1), cv::Point(x2, y2), red, 1);

        // 类别名
        std::string label =
            (box.class_id >= 0 && box.class_id < (int)COCO80_CLASS_.size())
                ? COCO80_CLASS_[box.class_id]
                : std::to_string(box.class_id);

        int baseline = 0;
        double font_scale = 0.4;
        int thickness = 1;
        cv::Size text_sz = cv::getTextSize(label.c_str(), cv::FONT_HERSHEY_SIMPLEX,
                                           font_scale, thickness, &baseline);

        int lbl_w = text_sz.width + 6;
        int lbl_h = text_sz.height + 4;
        int lbl_y = std::max(0, y1 - lbl_h);

        // 标签小框（紧贴检测框左上角上方）
        cv::rectangle(frame, cv::Point(x1, lbl_y),
                      cv::Point(x1 + lbl_w, y1), red, cv::FILLED);

        // 白色文字
        cv::putText(frame, label.c_str(), cv::Point(x1 + 2, lbl_y + text_sz.height),
                    cv::FONT_HERSHEY_SIMPLEX, font_scale, white, thickness);
    }
}



void PSQ_YOLOv8::draw_boxes(void* img , size_t w, size_t h ,std::vector<YoloBox> boxes)
{   

    if(boxes.empty())
    {   
        printf("【YOLOv8】draw_boxes: 无检测框\n");
        return;
    }

    cv::Mat frame((int)h, (int)w, CV_8UC3, img);

    for (const auto& box : boxes)
    {
        int x1 = std::max(0, (int)box.x1);
        int y1 = std::max(0, (int)box.y1);
        int x2 = std::min((int)w - 1, (int)box.x2);
        int y2 = std::min((int)h - 1, (int)box.y2);

        cv::Scalar red(0, 0, 255);
        cv::Scalar white(255, 255, 255);

        // 红色细框
        cv::rectangle(frame, cv::Point(x1, y1), cv::Point(x2, y2), red, 1);

        // 类别名
        std::string label =
            (box.class_id >= 0 && box.class_id < (int)COCO80_CLASS_.size())
                ? COCO80_CLASS_[box.class_id]
                : std::to_string(box.class_id);

        int baseline = 0;
        double font_scale = 0.4;
        int thickness = 1;
        cv::Size text_sz = cv::getTextSize(label.c_str(), cv::FONT_HERSHEY_SIMPLEX,
                                           font_scale, thickness, &baseline);

        int lbl_w = text_sz.width + 6;
        int lbl_h = text_sz.height + 4;
        int lbl_y = std::max(0, y1 - lbl_h);

        // 标签小框（紧贴检测框左上角上方）
        cv::rectangle(frame, cv::Point(x1, lbl_y),
                      cv::Point(x1 + lbl_w, y1), red, cv::FILLED);

        // 白色文字
        cv::putText(frame, label.c_str(), cv::Point(x1 + 2, lbl_y + text_sz.height),
                    cv::FONT_HERSHEY_SIMPLEX, font_scale, white, thickness);
    }
}