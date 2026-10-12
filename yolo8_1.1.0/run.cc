#include "strategy.hh"
#include <unistd.h>
#include <signal.h>
#include <fstream>
#include <string>
#include <cstring>
#include <cctype>
#include <memory>

// ==================== 配置结构体 ====================
struct SteConfig
{
    int    stream_num = 1;
    std::vector<std::string> cam_uris;
    std::string model_path;
    int    video_w = 1920;
    int    video_h = 1080;
    int    dst_w   = 800;
    int    dst_h   = 448;
    int    search_extimate = 0;
    float  iou_extimate    = 0.5f;
    int    http_port = 8080;
    bool   debug_noisy = true;
};

// ==================== 工具函数 ====================
static std::string trim(const std::string& s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

static SteConfig parse_set_cfg(const std::string& path)
{
    SteConfig cfg;
    std::ifstream f(path);
    if (!f.is_open())
    {
        printf("【cfg】无法打开配置文件: %s，使用默认参数\n", path.c_str());
        return cfg;
    }

    std::string line;
    while (std::getline(f, line))
    {
        // 去掉注释
        size_t cmt = line.find('#');
        if (cmt != std::string::npos)
            line = line.substr(0, cmt);

        // 找 =
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (key.empty() || val.empty())
            continue;

        std::string k(key);
        std::string v(val);

        if      (k == "stream_num")      cfg.stream_num      = std::stoi(v);
        else if (k == "model_path")      cfg.model_path      = v;
        else if (k == "video_w")         cfg.video_w         = std::stoi(v);
        else if (k == "video_h")         cfg.video_h         = std::stoi(v);
        else if (k == "dst_w")           cfg.dst_w           = std::stoi(v);
        else if (k == "dst_h")           cfg.dst_h           = std::stoi(v);
        else if (k == "search_extimate") cfg.search_extimate = std::stoi(v);
        else if (k == "iou_extimate")    cfg.iou_extimate    = std::stof(v);
        else if (k == "http_port")       cfg.http_port       = std::stoi(v);
        else if (k == "debug")           cfg.debug_noisy     = (v == "noisy");
        else if (k.rfind("cam", 0) == 0 && k.find("_uri") != std::string::npos)
        {
            // cam1_uri, cam2_uri, ... → 0-based index
            std::string num_str = k.substr(3, k.find("_uri") - 3);
            int idx = std::stoi(num_str) - 1;
            if (idx >= static_cast<int>(cfg.cam_uris.size()))
                cfg.cam_uris.resize(idx + 1);
            cfg.cam_uris[idx] = v;
        }
    }
    return cfg;
}

// ==================== 全局 ====================
static int running = 1;

void sigint_handler(int sig)
{
    (void)sig;
    printf("\n收到 Ctrl+C，准备退出...\n");
    running = 0;
}

// ==================== 主函数 ====================
int main(int argc, char* argv[])
{
    signal(SIGINT, sigint_handler);

    // ---- 1. 解析配置文件 ----
    const char* cfg_path = "set.cfg";
    if (argc >= 2)
        cfg_path = argv[1];

    SteConfig cfg = parse_set_cfg(cfg_path);

    // ---- 2. 校验 ----
    if (cfg.stream_num < 1 || cfg.stream_num > 5)
    {
        printf("【cfg】stream_num=%d 超出范围 (1-5)，强制修正为 1\n", cfg.stream_num);
        cfg.stream_num = 1;
    }
    if (static_cast<int>(cfg.cam_uris.size()) < cfg.stream_num)
    {
        printf("【cfg】URI 数量不足！需要 %d 路，实际配置 %zu 路\n",
               cfg.stream_num, cfg.cam_uris.size());
        return -1;
    }
    if (cfg.model_path.empty())
    {
        printf("【cfg】model_path 为空！请在 set.cfg 中配置\n");
        return -1;
    }

    // ---- 3. 打印配置概要 ----
    printf("===== 配置概要 =====\n");
    printf("  摄像头数量: %d\n", cfg.stream_num);
    for (int i = 0; i < cfg.stream_num; ++i)
        printf("    第 %d 路: %s\n", i + 1, cfg.cam_uris[i].c_str());
    printf("  模型路径  : %s\n", cfg.model_path.c_str());
    printf("  输入分辨率: %d x %d\n", cfg.video_w, cfg.video_h);
    printf("  推理分辨率: %d x %d\n", cfg.dst_w, cfg.dst_h);
    printf("  YOLOv8阈值: search=%d  iou=%.2f\n", cfg.search_extimate, cfg.iou_extimate);
    printf("  HTTP起始口: %d\n", cfg.http_port);
    printf("  调试模式  : %s\n", cfg.debug_noisy ? "noisy(吵闹)" : "quiet(安静)");
    printf("=====================\n\n");

    // ---- 4. 创建组件 ----

    // 共享组件（全局唯一）
    PSQ_rknn   rknn(cfg.model_path.c_str(), 1, 2);
    PSQ_rga    rga(NV12, cfg.video_w, cfg.video_h, RGB3x8, cfg.dst_w, cfg.dst_h);
    PSQ_YOLOv8 yolov8(
        rknn.get_attr(PSQ_RKNN_INPUT), rknn.get_attr(PSQ_RKNN_OUTPUT),
        rknn.get_mem(PSQ_RKNN_INPUT), rknn.get_mem(PSQ_RKNN_OUTPUT),
        cfg.search_extimate, cfg.iou_extimate,
        static_cast<float>(cfg.video_w), static_cast<float>(cfg.video_h)
    );

    

    // 每路独立组件（堆分配 + unique_ptr 管理，避免 vector 扩容时移动内部 mutex）
    std::vector<std::unique_ptr<PSQ_gts>>  gts_objs;
    std::vector<std::unique_ptr<PSQ_vpu>>  vpu_objs;
    std::vector<std::unique_ptr<PSQ_http>> http_objs;

    std::vector<PSQ_gts*>  gts_list;
    std::vector<PSQ_vpu*>  vpu_list;
    std::vector<PSQ_http*> http_list;

    for (int i = 0; i < cfg.stream_num; ++i)
    {
        gts_objs.push_back(std::make_unique<PSQ_gts>(cfg.cam_uris[i].c_str()));
        vpu_objs.push_back(std::make_unique<PSQ_vpu>(H265));
        http_objs.push_back(std::make_unique<PSQ_http>(cfg.http_port + i));

        gts_list.push_back(gts_objs.back().get());
        vpu_list.push_back(vpu_objs.back().get());
        http_list.push_back(http_objs.back().get());
    }

    


    // ---- 5. 启动策略管线 ----
    PSQ_strategic psq_strategic(
        cfg.stream_num,
        &gts_list, &vpu_list,
        &rga, &rknn, &yolov8, &http_list
    );

    psq_strategic.set_debug(cfg.debug_noisy);

    // ---- 5. 启动 USE 观察者 ----
    Observer_config obs_cfg;
    obs_cfg.manage_observer = true;
    obs_cfg.internal_USE = false;   // external 模式: U 从内核节点读取
    psq_strategic.set_observer(obs_cfg);

    psq_strategic.proces_gts_vpu();
    psq_strategic.proces_rga();
    psq_strategic.proces_rknn_yolo();
    psq_strategic.proces_http();

    printf("===== 管线已启动，%d 路摄像头运行中 =====\n", cfg.stream_num);
    printf("  HTTP 地址:\n");
    for (int i = 0; i < cfg.stream_num; ++i)
        printf("    第 %d 路 → http://localhost:%d\n", i + 1, cfg.http_port + i);

    // ---- 6. 主循环 ----
    while (running)
    {
        sleep(1);
    }

    // ---- 7. 优雅退出 ----
    printf("正在停止管线...\n");
    psq_strategic.end_http();
    psq_strategic.end_rknn_yolo();
    psq_strategic.end_rga();
    psq_strategic.end_gts_vpu();

    printf("管线已安全退出。\n");
    return 0;
}