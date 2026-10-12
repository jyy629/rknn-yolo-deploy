#include "yolov5.hh"
#include "font_cn.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <unistd.h>
#include <fcntl.h>
// ===== worker 线程 =====
void* yolov5_worker(void* arg) {
    HeadArg* head = (HeadArg*)arg;
    int work_cnt = 0;
    while (true) {
        pthread_mutex_lock(&head->mutex_work);
        while (!head->has_work && !head->stop)
            pthread_cond_wait(&head->cond_work, &head->mutex_work);
        if (head->stop) {
            pthread_mutex_unlock(&head->mutex_work);
            break;
        }
        head->owner->decode_one_head(head);
        if (++work_cnt % 30 == 0)
            fprintf(stderr, "worker decoded %d times, out_vec size=%zu\n",
                    work_cnt, head->out_vec->size());
        head->has_work  = false;
        head->work_done = true;
        pthread_cond_signal(&head->cond_done);
        pthread_mutex_unlock(&head->mutex_work);
    }
    return NULL;
}

// ===== 构造函数 =====
PSQ_YOLOv5::PSQ_YOLOv5(
    rknn_tensor_attr* input_attr,
    rknn_tensor_attr* output_attr,
    rknn_tensor_mem** input_mem,
    rknn_tensor_mem** output_mem,
    bool is_sigmoid,
    int  estimate,
    float iou_thresh)
{
    printf("-----------yolov5 初始化中----------------\n");

    estimate_   = estimate;
    iou_thresh_ = iou_thresh;

    input_attr_  = input_attr;
    output_attr_ = output_attr;
    input_mem_   = input_mem;
    output_mem_  = output_mem;

    model_info_.out_head_count = 3;
    model_info_.output_zp      = output_attr[0].zp;
    model_info_.output_scale   = output_attr[0].scale;
    model_info_.fmt            = output_attr[0].fmt;
    //    = input_attr[0].dims[1];

    heads_.resize(model_info_.out_head_count);
    head_out_vecs_.resize(model_info_.out_head_count);
    thread_ids_.resize(model_info_.out_head_count);
    final_dets_.reserve(1024);
    ranges_.reserve(100);

    for (int i = 0; i < model_info_.out_head_count; i++)
        head_out_vecs_[i].reserve(1024);

    printf("初始化中....【构建反量化表】");
    build_dequant_table(is_sigmoid);
    printf("转换表初始化完成!!!!!\n");

    printf("正在创建线程..........\n");
    create_threads();
    printf("线程创建完成!!!!!\n");
    printf("yolov5 初始化完成!!!!!\n");
}

// ===== 析构函数 =====
PSQ_YOLOv5::~PSQ_YOLOv5()
{
    printf("~yolov5 析构: 停止线程...\n");
    stop_threads();
    printf("~yolov5 析构完成\n");
}

// ===== 反量化表 =====
void PSQ_YOLOv5::build_dequant_table(bool is_sigmoid)
{
    int32_t zp    = model_info_.output_zp;
    float   scale = model_info_.output_scale;

    for (int i = 0; i < 256; i++) {
        int8_t raw   = (int8_t)(i - 128);
        float  logit = (raw - zp) * scale;
        if (is_sigmoid)
            dequant_table_[i] = logit;
        else
            dequant_table_[i] = 1.0f / (1.0f + expf(-logit));
    }
}

// ===== 创建线程 =====
void PSQ_YOLOv5::create_threads()
{
    int H = input_attr_[0].dims[1];
    int W = input_attr_[0].dims[2];

    for (int i = 0; i < model_info_.out_head_count; i++) {
        heads_[i].owner     = this;
        heads_[i].estimate  = (int8_t)estimate_;
        heads_[i].output    = (int8_t*)output_mem_[i]->virt_addr;
        heads_[i].out_vec   = &head_out_vecs_[i];
        heads_[i].has_work  = false;
        heads_[i].work_done = false;
        heads_[i].stop      = false;

        heads_[i].yolo_outshape[0] = output_attr_[i].dims[0];
        heads_[i].yolo_outshape[1] = output_attr_[i].dims[1];
        heads_[i].yolo_outshape[2] = output_attr_[i].dims[2];
        heads_[i].yolo_outshape[3] = output_attr_[i].dims[3];
        heads_[i].fmt              = output_attr_[i].fmt;
        heads_[i].fact_xy_stride   = W / output_attr_[i].dims[2];

        heads_[i].anchor_w[0] = YoloModelInfo::aw[i][0] * W / 640.0f;
        heads_[i].anchor_w[1] = YoloModelInfo::aw[i][1] * W / 640.0f;
        heads_[i].anchor_w[2] = YoloModelInfo::aw[i][2] * W / 640.0f;
        heads_[i].anchor_h[0] = YoloModelInfo::ah[i][0] * H / 640.0f;
        heads_[i].anchor_h[1] = YoloModelInfo::ah[i][1] * H / 640.0f;
        heads_[i].anchor_h[2] = YoloModelInfo::ah[i][2] * H / 640.0f;

        pthread_mutex_init(&heads_[i].mutex_work, NULL);
        pthread_cond_init(&heads_[i].cond_work, NULL);
        pthread_cond_init(&heads_[i].cond_done, NULL);
    }

    for (int i = 0; i < model_info_.out_head_count; i++)
        pthread_create(&thread_ids_[i], NULL, yolov5_worker, &heads_[i]);
}

// ===== 停止线程 =====
void PSQ_YOLOv5::stop_threads()
{
    for (int i = 0; i < model_info_.out_head_count; i++) {
        pthread_mutex_lock(&heads_[i].mutex_work);
        heads_[i].stop = true;
        pthread_cond_signal(&heads_[i].cond_work);
        pthread_mutex_unlock(&heads_[i].mutex_work);
        pthread_join(thread_ids_[i], NULL);
        pthread_mutex_destroy(&heads_[i].mutex_work);
        pthread_cond_destroy(&heads_[i].cond_work);
        pthread_cond_destroy(&heads_[i].cond_done);
    }
}

// ===== 每帧入口 =====
void PSQ_YOLOv5::manage()
{
    ranges_.clear();
    final_dets_.clear();
    for (int i = 0; i < model_info_.out_head_count; i++)
        head_out_vecs_[i].clear();

    dispatch_and_wait();
}

// ===== 令牌发放（条件变量版）=====
void PSQ_YOLOv5::dispatch_and_wait()
{
    for (int i = model_info_.out_head_count - 1; i >= 0; i--) {
        pthread_mutex_lock(&heads_[i].mutex_work);
        heads_[i].has_work  = true;
        heads_[i].work_done = false;
        pthread_cond_signal(&heads_[i].cond_work);
        while (!heads_[i].work_done)
            pthread_cond_wait(&heads_[i].cond_done, &heads_[i].mutex_work);
        pthread_mutex_unlock(&heads_[i].mutex_work);
    }
    all_nms();
}

// ===== 坐标映射 =====
float PSQ_YOLOv5::out_fact_figure(float value, int row_or_line, int stride, int xy_or_wh, float anchor_hw)
{
    if (xy_or_wh == 0)
        return (2.0f * value - 0.5f + row_or_line) * stride;
    if (xy_or_wh == 1)
        return (2.0f * value) * (2.0f * value) * anchor_hw;
    return -1;
}

// ===== NEON 解码 =====
void PSQ_YOLOv5::decode_one_head(void* arg)
{
    HeadArg* head = (HeadArg*)arg;

    int fmt = head->fmt;
    int C, H, W;
    if (fmt == RKNN_TENSOR_NCHW) {
        C = head->yolo_outshape[1];
        H = head->yolo_outshape[2];
        W = head->yolo_outshape[3];
    } else {
        H = head->yolo_outshape[1];
        W = head->yolo_outshape[2];
        C = head->yolo_outshape[3];
    }

    int8_t* out          = head->output;
    int     num_classes  = C / 3 - 5;
    int     group_stride = C / 3;

    if (fmt == RKNN_TENSOR_NCHW) {
        const int8x16_t vth = vdupq_n_s8(head->estimate);
        const size_t    HW  = (size_t)H * W;

        for (int g = 0; g < 3; g++) {
            const int obj_layer  = g * group_stride + 4;
            const int cls_start  = g * group_stride + 5;
            const int xywh_base  = g * group_stride;

            const int8_t* obj_plane = out + (size_t)obj_layer * HW;

            auto process = [&](int c) {
                int8_t obj_val = *(obj_plane + (size_t)0 * HW + 0 * W + c);  // placeholder, will be fixed
                // Actually we need r and c. Let me rewrite this properly.
            };

            for (int r = 0; r < H; r++) {
                const int8_t* orow = obj_plane + (size_t)r * W;

                auto process_cell = [&](int c) {
                    int8_t obj_val = orow[c];

                    int8_t max_cls = -128, max_cls_id = 0;
                    const int8_t* cls_ptr = out + (size_t)cls_start * HW + (size_t)r * W + c;
                    for (int k = 0; k < num_classes; k++) {
                        int8_t v = cls_ptr[(size_t)k * HW];
                        if (v > max_cls) { max_cls = v; max_cls_id = k; }
                    }

                    const int8_t* xywh_ptr = out + (size_t)xywh_base * HW + (size_t)r * W + c;
                    YoloBox det;
                    det.x   = out_fact_figure(dequant_table_[xywh_ptr[0]        + 128], c, head->fact_xy_stride, 0, 0);
                    det.y   = out_fact_figure(dequant_table_[xywh_ptr[HW]       + 128], r, head->fact_xy_stride, 0, 0);
                    det.w   = out_fact_figure(dequant_table_[xywh_ptr[2 * HW]   + 128], 0, 0, 1, head->anchor_w[g]);
                    det.h   = out_fact_figure(dequant_table_[xywh_ptr[3 * HW]   + 128], 0, 0, 1, head->anchor_h[g]);
                    det.conf      = dequant_table_[obj_val + 128] * dequant_table_[max_cls + 128];
                    det.class_id  = max_cls_id;
                    head->out_vec->push_back(det);
                };

                int c = 0;
                for (; c + 16 <= W; c += 16) {
                    uint8x16_t hit = vcgtq_s8(vld1q_s8(orow + c), vth);
                    uint64_t m0 = vgetq_lane_u64(vreinterpretq_u64_u8(hit), 0);
                    uint64_t m1 = vgetq_lane_u64(vreinterpretq_u64_u8(hit), 1);

                    uint64_t bits = m0 & 0x8080808080808080ULL;
                    while (bits) {
                        int k = __builtin_ctzll(bits) >> 3;
                        process_cell(c + k);
                        bits &= bits - 1;
                    }
                    bits = m1 & 0x8080808080808080ULL;
                    while (bits) {
                        int k = __builtin_ctzll(bits) >> 3;
                        process_cell(c + 8 + k);
                        bits &= bits - 1;
                    }
                }
                for (; c < W; c++) {
                    if (orow[c] > head->estimate) process_cell(c);
                }
            }
        }
    } else {
        auto get = [&](int ch, int row, int col) -> int8_t {
            return out[((size_t)row * W + col) * C + ch];
        };
        for (int g = 0; g < 3; g++) {
            int obj_layer = g * group_stride + 4;
            int cls_start = g * group_stride + 5;
            int xywh_base = g * group_stride;
            for (int r = 0; r < H; r++) {
                for (int c = 0; c < W; c++) {
                    int8_t obj_val = get(obj_layer, r, c);
                    if (obj_val <= head->estimate) continue;

                    int8_t max_cls = -128, max_cls_id = 0;
                    for (int k = 0; k < num_classes; k++) {
                        int8_t cls_val = get(cls_start + k, r, c);
                        if (cls_val > max_cls) { max_cls = cls_val; max_cls_id = k; }
                    }

                    YoloBox det;
                    det.x   = out_fact_figure(dequant_table_[get(xywh_base + 0, r, c) + 128], c, head->fact_xy_stride, 0, 0);
                    det.y   = out_fact_figure(dequant_table_[get(xywh_base + 1, r, c) + 128], r, head->fact_xy_stride, 0, 0);
                    det.w   = out_fact_figure(dequant_table_[get(xywh_base + 2, r, c) + 128], 0, 0, 1, head->anchor_w[g]);
                    det.h   = out_fact_figure(dequant_table_[get(xywh_base + 3, r, c) + 128], 0, 0, 1, head->anchor_h[g]);
                    det.conf      = dequant_table_[obj_val + 128] * dequant_table_[max_cls + 128];
                    det.class_id  = max_cls_id;
                    head->out_vec->push_back(det);
                }
            }
        }
    }
}

// ===== SoA 转换 =====
void PSQ_YOLOv5::to_soa(const YoloBox* base, int size,
                         float* x1, float* y1, float* x2, float* y2, float* area)
{
    for (int i = 0; i < size; i++) {
        float hw = base[i].w * 0.5f;
        float hh = base[i].h * 0.5f;
        x1[i]   = base[i].x - hw;
        x2[i]   = base[i].x + hw;
        y1[i]   = base[i].y - hh;
        y2[i]   = base[i].y + hh;
        area[i] = base[i].w * base[i].h;
    }
}

// ===== NEON NMS 段 =====
void PSQ_YOLOv5::nms_segment(uint32_t* keep, int base, int size,
                              const float* x1, const float* y1,
                              const float* x2, const float* y2,
                              const float* area, float thresh)
{
    if (size < 2) return;

    const float32x4_t vth = vdupq_n_f32(thresh);
    const float32x4_t v0  = vdupq_n_f32(0.f);

    for (int a = 0; a < size; a++) {
        const int ai = base + a;
        if (!((keep[ai >> 5] >> (ai & 31)) & 1u)) continue;

        const float32x4_t bx1 = vdupq_n_f32(x1[a]);
        const float32x4_t by1 = vdupq_n_f32(y1[a]);
        const float32x4_t bx2 = vdupq_n_f32(x2[a]);
        const float32x4_t by2 = vdupq_n_f32(y2[a]);
        const float32x4_t ba  = vdupq_n_f32(area[a]);

        auto scalar_iou = [&](int t) {
            const int bt = base + t;
            if (!((keep[bt >> 5] >> (bt & 31)) & 1u)) return;
            float iw = std::min(x2[a], x2[t]) - std::max(x1[a], x1[t]);
            float ih = std::min(y2[a], y2[t]) - std::max(y1[a], y1[t]);
            if (iw > 0.f && ih > 0.f) {
                float inter = iw * ih;
                float uni   = area[a] + area[t] - inter;
                if (inter > uni * thresh)
                    keep[bt >> 5] &= ~(1u << (bt & 31));
            }
        };

        int j = a + 1;
        for (; j + 4 <= size; j += 4) {
            const int bi = base + j;
            const int w  = bi >> 5;
            const int sh = bi & 31;

            if (sh > 28) {
                for (int t = j; t < j + 4; t++) scalar_iou(t);
                continue;
            }

            if (((keep[w] >> sh) & 0xFu) == 0) continue;

            const float32x4_t ix1 = vmaxq_f32(bx1, vld1q_f32(x1 + j));
            const float32x4_t iy1 = vmaxq_f32(by1, vld1q_f32(y1 + j));
            const float32x4_t ix2 = vminq_f32(bx2, vld1q_f32(x2 + j));
            const float32x4_t iy2 = vminq_f32(by2, vld1q_f32(y2 + j));

            const float32x4_t iw    = vmaxq_f32(vsubq_f32(ix2, ix1), v0);
            const float32x4_t ih    = vmaxq_f32(vsubq_f32(iy2, iy1), v0);
            const float32x4_t inter = vmulq_f32(iw, ih);

            const float32x4_t uni  = vsubq_f32(vaddq_f32(ba, vld1q_f32(area + j)), inter);
            const uint32x4_t  kill = vcgtq_f32(inter, vmulq_f32(uni, vth));

            const uint32_t k4 =
                (vgetq_lane_u32(kill, 0) & 1u) |
                ((vgetq_lane_u32(kill, 1) & 1u) << 1) |
                ((vgetq_lane_u32(kill, 2) & 1u) << 2) |
                ((vgetq_lane_u32(kill, 3) & 1u) << 3);
            keep[w] &= ~(k4 << sh);
        }
        for (; j < size; j++) scalar_iou(j);
    }
}

// ===== NMS 入口 =====
void PSQ_YOLOv5::nms_iou(uint32_t* keep_bits, std::vector<YoloBox>& all_dets)
{
    const int N = (int)all_dets.size();
    if (N <= 0) return;

    std::fill(keep_bits, keep_bits + ((N + 31) >> 5), 0xFFFFFFFFu);

    alignas(16) float sx1[YoloModelInfo::IOU_MAX_DET], sy1[YoloModelInfo::IOU_MAX_DET];
    alignas(16) float sx2[YoloModelInfo::IOU_MAX_DET], sy2[YoloModelInfo::IOU_MAX_DET];
    alignas(16) float sarea[YoloModelInfo::IOU_MAX_DET];
    std::vector<float> vx1, vy1, vx2, vy2, varea;

    float *x1, *y1, *x2, *y2, *area;
    if (N <= YoloModelInfo::IOU_MAX_DET) {
        x1 = sx1; y1 = sy1; x2 = sx2; y2 = sy2; area = sarea;
    } else {
        vx1.resize(N); vy1.resize(N); vx2.resize(N); vy2.resize(N); varea.resize(N);
        x1 = vx1.data(); y1 = vy1.data(); x2 = vx2.data(); y2 = vy2.data(); area = varea.data();
    }
    to_soa(all_dets.data(), N, x1, y1, x2, y2, area);

    for (const ClassRange& r : ranges_) {
        int size = r.end - r.start + 1;
        if (size < 2) continue;
        nms_segment(keep_bits, r.start, size,
                    x1 + r.start, y1 + r.start, x2 + r.start, y2 + r.start, area + r.start,
                    iou_thresh_);
    }
}

// ===== 全量 NMS =====
void PSQ_YOLOv5::all_nms()
{
    std::vector<YoloBox> all_dets;
    all_dets.reserve(1024);
    for (int i = 0; i < model_info_.out_head_count; i++)
        all_dets.insert(all_dets.end(), head_out_vecs_[i].begin(), head_out_vecs_[i].end());

    std::sort(all_dets.begin(), all_dets.end(),
        [](const YoloBox& a, const YoloBox& b) {
            if (a.class_id != b.class_id)
                return a.class_id < b.class_id;
            return a.conf > b.conf;
        });

    for (size_t i = 0; i < all_dets.size(); ) {
        int cid = all_dets[i].class_id;
        size_t j = i;
        while (j < all_dets.size() && all_dets[j].class_id == cid) ++j;
        ranges_.push_back({cid, (int)i, (int)j - 1});
        i = j;
    }

    std::vector<uint32_t> keep_bits((all_dets.size() + 31) >> 5, 0xFFFFFFFFu);
    nms_iou(keep_bits.data(), all_dets);

    for (size_t i = 0; i < all_dets.size(); i++)
        if ((keep_bits[i >> 5] >> (i & 31)) & 1u)
            final_dets_.push_back(all_dets[i]);
}

// ===== 画框 + 中文标签 =====
void PSQ_YOLOv5::draw(uint8_t* user_buf, int width, int height)
{
    const int stride    = width * 3;
    const int thickness = 2;

    auto draw_hline = [&](int y, int x1, int x2) {
        uint8_t* row = user_buf + y * stride;
        for (int x = x1; x <= x2; x++) {
            row[x * 3 + 0] = 255;
            row[x * 3 + 1] = 0;
            row[x * 3 + 2] = 0;
        }
    };

    auto draw_vline = [&](int x, int y1, int y2) {
        for (int y = y1; y <= y2; y++) {
            uint8_t* row = user_buf + y * stride;
            row[x * 3 + 0] = 255;
            row[x * 3 + 1] = 0;
            row[x * 3 + 2] = 0;
        }
    };

    auto draw_char = [&](int ch_idx, int cx, int cy) {
        if (ch_idx >= FONT_CN_COUNT) return;
        const uint8_t* bits = font_cn_data[ch_idx];
        for (int fy = 0; fy < FONT_CN_CHAR_H; fy++) {
            int py = cy + fy;
            if (py < 0 || py >= height) continue;
            uint8_t* row = user_buf + py * stride;
            for (int fx = 0; fx < FONT_CN_CHAR_W; fx++) {
                int px = cx + fx;
                if (px < 0 || px >= width) continue;
                int byte_off = fy * FONT_CN_BYTES_PER_ROW + fx / 8;
                int bit_off  = 7 - (fx % 8);
                if (bits[byte_off] & (1 << bit_off)) {
                    row[px * 3 + 0] = 255;
                    row[px * 3 + 1] = 255;
                    row[px * 3 + 2] = 255;
                }
            }
        }
    };

    for (size_t i = 0; i < final_dets_.size(); i++) {
        const YoloBox& d = final_dets_[i];

        int x1 = (int)(d.x - d.w * 0.5f);
        int y1 = (int)(d.y - d.h * 0.5f);
        int x2 = (int)(d.x + d.w * 0.5f);
        int y2 = (int)(d.y + d.h * 0.5f);

        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x2 >= width)  x2 = width - 1;
        if (y2 >= height) y2 = height - 1;
        if (x2 < x1 || y2 < y1) continue;

        for (int t = 0; t < thickness && (y1 + t) < height; t++)
            draw_hline(y1 + t, x1, x2);
        for (int t = 0; t < thickness && (y2 - t) >= 0; t++)
            draw_hline(y2 - t, x1, x2);
        for (int t = 0; t < thickness && (x1 + t) < width; t++)
            draw_vline(x1 + t, y1, y2);
        for (int t = 0; t < thickness && (x2 - t) >= 0; t++)
            draw_vline(x2 - t, y1, y2);

        int cls = (d.class_id >= 0 && d.class_id < 80) ? d.class_id : 0;
        const auto& lbl = font_cn_labels[cls];
        int lbl_w = lbl.count * (FONT_CN_CHAR_W + 1);
        int lbl_h = FONT_CN_CHAR_H + 2;
        int tx = x1;
        int ty = y1 - lbl_h;
        if (ty < 0) ty = y1 + 2;

        for (int by = ty; by < ty + lbl_h && by < height; by++) {
            uint8_t* row = user_buf + by * stride;
            for (int bx = tx; bx < tx + lbl_w && bx < width; bx++) {
                row[bx * 3 + 0] = 0;
                row[bx * 3 + 1] = 0;
                row[bx * 3 + 2] = 0;
            }
        }

        int cx = tx + 1;
        for (int k = 0; k < lbl.count; k++) {
            draw_char(lbl.idx[k], cx, ty + 1);
            cx += FONT_CN_CHAR_W + 1;
        }
    }
}

int PSQ_YOLOv5::examine(examine_status status)
{
    int fd = open("./examine_log.txt", O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        perror("open examine_log.txt");
        return -1;
    }

    switch (status) {
    case finaly_vector: {
        dprintf(fd, "========== 最终检测结果 (final_dets_) ==========\n");
        dprintf(fd, "  共 %zu 个检测框\n", final_dets_.size());
        for (size_t i = 0; i < final_dets_.size(); i++) {
            const YoloBox& d = final_dets_[i];
            dprintf(fd, "  [%2zu] cls=%2d  conf=%.4f  "
                    "x=%.1f y=%.1f w=%.1f h=%.1f  "
                    "(x1=%.1f y1=%.1f x2=%.1f y2=%.1f)\n",
                    i, d.class_id, d.conf,
                    d.x, d.y, d.w, d.h,
                    d.x - d.w * 0.5f, d.y - d.h * 0.5f,
                    d.x + d.w * 0.5f, d.y + d.h * 0.5f);
        }
        dprintf(fd, "================================================\n\n");
        break;
    }

    case headout_vector: {
        dprintf(fd, "========== 各输出头原始检测 (head_out_vecs_) ==========\n");
        int total = 0;
        for (int h = 0; h < model_info_.out_head_count; h++) {
            const auto& vec = head_out_vecs_[h];
            dprintf(fd, "  --- head[%d]: %zu 个候选框 ---\n", h, vec.size());
            for (size_t i = 0; i < vec.size(); i++) {
                const YoloBox& d = vec[i];
                dprintf(fd, "    [%2zu] cls=%2d  conf=%.4f  "
                        "x=%.1f y=%.1f w=%.1f h=%.1f\n",
                        i, d.class_id, d.conf, d.x, d.y, d.w, d.h);
            }
            total += (int)vec.size();
        }
        dprintf(fd, "  总计: %d 个候选框\n", total);
        dprintf(fd, "=======================================================\n\n");
        break;
    }

    case dequantiza_list: {
        dprintf(fd, "========== 反量化表 (dequant_table_) ==========\n");
        dprintf(fd, "  zp=%d  scale=%.6f\n",
                model_info_.output_zp, model_info_.output_scale);
        for (int i = 0; i < 256; i++) {
            dprintf(fd, "  [%3d] -> %.6f", i - 128, dequant_table_[i]);
            if ((i + 1) % 4 == 0) dprintf(fd, "\n");
        }
        dprintf(fd, "================================================\n\n");
        break;
    }

    default:
        dprintf(fd, "examine: 未知状态 %d\n", (int)status);
        break;
    }

    close(fd);
    return 0;
}