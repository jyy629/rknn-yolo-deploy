#ifndef OBSERVER_HH
#define OBSERVER_HH

#include <stdio.h>
#include <chrono>
#include <pthread.h>
#include <unistd.h>
#include <iostream>
#include <vector>


#define TIME_COST(code) do {                                  \
    auto _start = std::chrono::steady_clock::now();           \
    code;                                                     \
    auto _end = std::chrono::steady_clock::now();             \
    double _sec = std::chrono::duration<double>(_end - _start).count(); \
    std::cout << "[TIME] " #code " 耗时: " << _sec << " 秒" << std::endl; \
} while(0)

//==================== 作用域 ======================
using time_p = std::chrono::steady_clock::time_point;
using nano_sec = std::chrono::nanoseconds;

//==================== 设备类型 ======================
enum Device_name
{
    NPU,
    VPU,
    RGA,
};

//==================== 配置 ======================
struct Observer_config
{   
    bool manage_observer = false;
    bool internal_USE = false;
    bool internal_rga_USE = false;
    bool internal_vpu_USE = false;
    bool internal_npu_USE = false;
};

//==================== 表单相关 ======================
struct form_pross_info
{
    Device_name device;
    nano_sec now_work_time;
    int all_error_num = 0;
};

//==================== 缓存大小 ======================
struct all_now_cache_size
{
    std::vector<size_t> gts_vpu_cache_size;
    size_t rga_cache_size = 0;
    size_t rknn_yolo_cache_size = 0;
};

//==================== 基础结构 ======================
struct calculate_result
{
    double mean_U = 0.0;
    double moment_U = 0.0;
    int all_error_num = 0;
};

struct pthread_out
{
    calculate_result rga_figure;
    calculate_result vpu_figure;
    calculate_result npu_figure;
    all_now_cache_size cache_info;
};

//==================== 前置声明 ======================
class PSQ_USE_observer;
class PSQ_strategic;
void* USE_figure_out(void* arg);

//==================== 各设备USE信息 ======================
struct npu_USE_info
{
    const char* U = "/sys/kernel/debug/rknpu/load";
    nano_sec history_work_time;
    nano_sec now_work_time;
    int all_error_num = 0;
};

struct rga_USE_info
{
    const char* U = "/sys/kernel/debug/rkrga/load";
    nano_sec history_work_time;
    nano_sec now_work_time;
    int all_error_num = 0;
};

struct vpu_USE_info
{
    const char* U = "/proc/mpp_service/load";
    nano_sec history_work_time;
    nano_sec now_work_time;
    int all_error_num = 0;
};

//==================== 线程包 ======================
struct pthread_backage
{
    PSQ_USE_observer* observer_class;
    pthread_out* pth_out;
};

//==================== 内部信息相关 ======================
struct Internal_info
{
    time_p all_system_start_time;
    nano_sec now_system_work_time;

    pthread_t pth_id;
    bool running = false;
    pthread_backage* pth_backage = NULL;
};

//==================== 观察者类 ======================
class PSQ_USE_observer
{
public:
    PSQ_USE_observer(Observer_config config, PSQ_strategic* psq_strategic)
    {
        this->observer_config = config;
        this->psq_strategic = psq_strategic;

        // 清零历史工作时间和错误计数（保留 U 路径默认值）
        rga_USE.history_work_time = nano_sec::zero();
        rga_USE.all_error_num = 0;
        vpu_USE.history_work_time = nano_sec::zero();
        vpu_USE.all_error_num = 0;
        npu_USE.history_work_time = nano_sec::zero();
        npu_USE.all_error_num = 0;

        // 清零输出结构的标量字段（vector 有 resize 处理，calculate_result 有默认值）
        pth_out.cache_info.rga_cache_size = 0;
        pth_out.cache_info.rknn_yolo_cache_size = 0;

        if (config.manage_observer && config.internal_USE == false)
        {
            system("echo 1000 > /proc/mpp_service/load_interval");
        }

        internal_info.all_system_start_time = std::chrono::steady_clock::now();

        this->internal_info.pth_backage = new pthread_backage();
        this->internal_info.pth_backage->observer_class = this;
        this->internal_info.pth_backage->pth_out = &pth_out;

        this->internal_info.running = true;

        int ret = pthread_create(
            &internal_info.pth_id,
            NULL,
            USE_figure_out,
            internal_info.pth_backage
        );
    }

    ~PSQ_USE_observer()
    {
        internal_info.running = false;
        pthread_join(internal_info.pth_id, NULL);
        delete internal_info.pth_backage;
    }

    void sent_to_observer(form_pross_info info)
    {   
        switch (info.device)
        {
        case RGA:
            rga_USE.now_work_time = info.now_work_time;
            rga_USE.history_work_time += info.now_work_time;
            rga_USE.all_error_num += info.all_error_num;
            break;
        
        case VPU:
            vpu_USE.now_work_time = info.now_work_time;
            vpu_USE.history_work_time += info.now_work_time;
            vpu_USE.all_error_num += info.all_error_num;
            break;

        case NPU:
            npu_USE.now_work_time = info.now_work_time;
            npu_USE.history_work_time += info.now_work_time;
            npu_USE.all_error_num += info.all_error_num;
            break;

        default:
            break;
        }
    }

    nano_sec get_now_system_time()
    {
        time_p end = std::chrono::steady_clock::now();
        internal_info.now_system_work_time = end - internal_info.all_system_start_time;
        return internal_info.now_system_work_time;
    }

    Internal_info internal_info;
    Observer_config observer_config;
    PSQ_strategic* psq_strategic;

    rga_USE_info rga_USE;
    vpu_USE_info vpu_USE;
    npu_USE_info npu_USE;

    pthread_out pth_out;

friend void* USE_figure_out(void* arg);
template<typename T> friend calculate_result figure_out(PSQ_USE_observer*, T);
};

//==================== 方法函数 ======================
template<typename T>
calculate_result figure_out(PSQ_USE_observer* observer_class, T device)
{
    nano_sec system_work = observer_class->get_now_system_time();
    nano_sec device_work_time = device.history_work_time;
    nano_sec device_moment_work_time = device.now_work_time;

    double mean_U = 100.0 * double(device_work_time.count()) / double(system_work.count());
    double moment_U = 100.0 * double(device_moment_work_time.count()) / 1e9;

    calculate_result result = {mean_U, moment_U};
    result.all_error_num = device.all_error_num;
    return result;
}
//==================== 线程函数 ======================
void* USE_figure_out(void* arg)
{
    pthread_backage* package = (pthread_backage*)arg;

    while (package->observer_class->internal_info.running)
    {
        sleep(1);

        PSQ_USE_observer* obs = package->observer_class;
        FILE* fp = fopen("/tmp/use_monitor", "w");
        if (!fp) continue;

        long ts = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        if (obs->observer_config.internal_USE)
        {
            package->pth_out->rga_figure = figure_out(obs, obs->rga_USE);
            package->pth_out->vpu_figure = figure_out(obs, obs->vpu_USE);
            package->pth_out->npu_figure = figure_out(obs, obs->npu_USE);

            fprintf(fp, "mode=internal\n");
            fprintf(fp, "ts=%ld\n\n", ts);

            fprintf(fp, "rga_U_avg=%.1f\n", package->pth_out->rga_figure.mean_U);
            fprintf(fp, "rga_U_now=%.1f\n", package->pth_out->rga_figure.moment_U);
            fprintf(fp, "rga_S=%d\n", package->pth_out->cache_info.rga_cache_size);
            fprintf(fp, "rga_E=%d\n\n", obs->rga_USE.all_error_num);

            fprintf(fp, "vpu_U_avg=%.1f\n", package->pth_out->vpu_figure.mean_U);
            fprintf(fp, "vpu_U_now=%.1f\n", package->pth_out->vpu_figure.moment_U);
            fprintf(fp, "vpu_E=%d\n", obs->vpu_USE.all_error_num);
            fprintf(fp, "vpu_S=");
            auto& vs = package->pth_out->cache_info.gts_vpu_cache_size;
            for (size_t i = 0; i < vs.size(); i++) {
                fprintf(fp, "%s%zu", i ? "," : "", vs[i]);
            }
            fprintf(fp, "\n\n");

            fprintf(fp, "npu_U_avg=%.1f\n", package->pth_out->npu_figure.mean_U);
            fprintf(fp, "npu_U_now=%.1f\n", package->pth_out->npu_figure.moment_U);
            fprintf(fp, "npu_S=%zu\n", package->pth_out->cache_info.rknn_yolo_cache_size);
            fprintf(fp, "npu_E=%d\n", obs->npu_USE.all_error_num);
        }
        else
        {
            double rga_U = -1, vpu_U = -1, npu_U = -1;

            // RGA: 多行，load = 在中间行，跳过标题行 "=== load ==="
            {
                FILE* f = fopen(obs->rga_USE.U, "r");
                if (f) { char buf[1024] = {0}; fread(buf, 1, sizeof(buf)-1, f); fclose(f);
                    char* p = strstr(buf, "load =");
                    if (p) { p = strstr(p + 1, "load =");  // 跳过标题行，取第二个
                    if (p) sscanf(p, "load = %lf%%", &rga_U); } }
            }
            // NPU: "NPU load: 68%"
            {
                FILE* f = fopen(obs->npu_USE.U, "r");
                if (f) { char buf[64] = {0}; fread(buf, 1, sizeof(buf)-1, f); fclose(f);
                    sscanf(buf, "NPU load: %lf%%", &npu_U); }
            }
            // VPU: 多行，rkvdec 行 "rkvdec           load:   2.51%"
            {
                FILE* f = fopen(obs->vpu_USE.U, "r");
                if (f) { char buf[2048] = {0}; fread(buf, 1, sizeof(buf)-1, f); fclose(f);
                    char* p = strstr(buf, "rkvdec");
                    if (p) { p = strstr(p, "load:"); if (p) sscanf(p, "load: %lf%%", &vpu_U); } }
            }

            fprintf(fp, "mode=external\n");
            fprintf(fp, "ts=%ld\n\n", ts);

            fprintf(fp, "rga_U=%.1f\n", rga_U);
            fprintf(fp, "rga_S=%zu\n", package->pth_out->cache_info.rga_cache_size);
            fprintf(fp, "rga_E=%d\n\n", obs->rga_USE.all_error_num);

            fprintf(fp, "vpu_U=%.1f\n", vpu_U);
            fprintf(fp, "vpu_E=%d\n", obs->vpu_USE.all_error_num);
            fprintf(fp, "vpu_S=");
            auto& vs = package->pth_out->cache_info.gts_vpu_cache_size;
            for (size_t i = 0; i < vs.size(); i++) {
                fprintf(fp, "%s%zu", i ? "," : "", vs[i]);
            }
            fprintf(fp, "\n\n");

            fprintf(fp, "npu_U=%.1f\n", npu_U);
            fprintf(fp, "npu_S=%zu\n", package->pth_out->cache_info.rknn_yolo_cache_size);
            fprintf(fp, "npu_E=%d\n", obs->npu_USE.all_error_num);
        }

        fclose(fp);
    }

    return nullptr;
}

#endif