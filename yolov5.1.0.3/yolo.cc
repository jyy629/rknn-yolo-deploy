#include "yolov5.hh"
#include "v4l2.hh"
#include "rga.hh"
#include "http.hh"
#include <check_error.hh>
#include <rknpu2/rknn_api.h>
#include <csignal>
#include <cstring>
#include <cstdio>
#include <cstdlib>

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    char* model_path = argv[1];

    rknn_context context;
    int ret = rknn_init(&context, model_path, 0, 0, NULL);
    check_error(ret);

    rknn_tensor_attr input_attr[1], output_attr[3];
    memset(input_attr,  0, sizeof(input_attr));
    memset(output_attr, 0, sizeof(output_attr));

    input_attr[0].index = 0;
    ret = rknn_query(context, RKNN_QUERY_INPUT_ATTR, &input_attr[0], sizeof(rknn_tensor_attr));
    check_error(ret);

    for (int i = 0; i < 3; i++) {
        output_attr[i].index = i;
        ret = rknn_query(context, RKNN_QUERY_OUTPUT_ATTR, &output_attr[i], sizeof(rknn_tensor_attr));
        check_error(ret);
    }

    rknn_tensor_mem** input_mem  = (rknn_tensor_mem**)malloc(sizeof(rknn_tensor_mem*) * 1);
    rknn_tensor_mem** output_mem = (rknn_tensor_mem**)malloc(sizeof(rknn_tensor_mem*) * 3);
    if (!input_mem || !output_mem) return -1;
    memset(input_mem,  0, sizeof(rknn_tensor_mem*) * 1);
    memset(output_mem, 0, sizeof(rknn_tensor_mem*) * 3);

    input_mem[0] = rknn_create_mem(context, input_attr[0].size);
    check_error(ret);
    rknn_set_io_mem(context, input_mem[0], &input_attr[0]);

    for (int i = 0; i < 3; i++) {
        output_mem[i] = rknn_create_mem(context, output_attr[i].size);
        check_error(ret);
        rknn_set_io_mem(context, output_mem[i], &output_attr[i]);
    }

    PSQ_v4l2 v4l2(const_cast<char*>("/dev/video0"), 640, 480, 30, true);

    PSQ_rga  rga(&v4l2, 640, 640);
    PSQ_http http(8080, rga);

    PSQ_YOLOv5 yolov5(
        input_attr, output_attr,
        input_mem, output_mem,
        true,   // is_sigmoid
        -70,    // estimate
        0.45f   // iou_thresh
    );

    while (g_running) {
        int idx = v4l2.v4l2_get_frame();
        if (idx == -2) { usleep(5000); continue; }
        if (idx == -1) break;

        rga.yuyv2rgb(idx);

        memcpy(input_mem[0]->virt_addr, rga.im_dma_va, input_mem[0]->size);

        rknn_mem_sync(context, input_mem[0], RKNN_MEMORY_SYNC_TO_DEVICE);
        ret = rknn_run(context, NULL);
        check_error(ret);

        for (int i = 0; i < 3; i++)
            rknn_mem_sync(context, output_mem[i], RKNN_MEMORY_SYNC_FROM_DEVICE);

        yolov5.manage();
        static int fc = 0;
        if (++fc % 30 == 0) {
            printf("frame %d\n", fc);
            fflush(stdout);
            yolov5.examine(headout_vector);
        }
        yolov5.draw((uint8_t*)rga.im_dma_va, (int)rga.WIDTH, (int)rga.HEIGHT);

        http.frame_ready();
    }

    printf("\n===== 程序退出，开始析构 =====\n");

    for (int i = 0; i < 3; i++) rknn_destroy_mem(context, output_mem[i]);
    rknn_destroy_mem(context, input_mem[0]);
    free(input_mem);
    free(output_mem);
    rknn_destroy(context);

    printf("===== RKNN 资源释放完成 =====\n");
    return 0;
}