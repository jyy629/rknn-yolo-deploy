#ifndef STRATEGY_HH
#define STRATEGY_HH

#include <gts_0.0.0.hh>
#include <vpu_0.0.1.hh>
#include <rga_0.0.0.hh>
#include <rkn_0.0.0.hh>
#include <yolov8_0.0.0.hh>
#include <http_0.0.0.hh>
#include <observer.hh>

class PSQ_strategic;

#include <vector>

#include <creat_cma.h>
#include <mutex>
#include <condition_variable>
#include <pthread.h>
//====================【缓存区数据结构】=========================

struct gts_vpu_package_detail
{
    int video_id = 0;           //标识哪一个摄像头
    after_vpu_frame_info af_frame;
};

struct rga_cuerent_package_detail
{
    int video_id = 0;
    //bool rga_flag = false;

    std::vector<uint8_t> temp;
    size_t temp_size = 0;
    size_t temp_w = 0;
    size_t temp_h = 0;

    std::vector<uint8_t> destination;
    size_t dst_size = 0;

};

struct rknn_yolo_package_detail
{
    int video_id = 0;
    bool waiting_task = false;
    std::vector<YoloBox> recognize_boxs;

    std::vector<uint8_t> temp;
    size_t temp_w = 0;
    size_t temp_h = 0;
    size_t temp_size = 0;

    std::vector<uint8_t> destination;
    size_t dst_size = 0;

};

struct http_package_detail
{
    //目前不需要，后续再考虑
};

//====================【方法函数】（环形缓存区丢弃帧的回调函数）====================
void* ex_vpu_result(gts_vpu_package_detail& detail)
{   
    after_vpu_frame_info af_info = detail.af_frame;

    if (af_info.frm == NULL) return nullptr;  // 环不满时空包，跳过

    MPP_RET ret = mpp_frame_deinit(&af_info.frm);
    if(ret != MPP_OK)
    {
        printf("【vpu】{result}:release_fail pts=%lld ret=%d\n", af_info.pts, ret);
        return nullptr;
    }
    return nullptr;
}
//====================【环形缓存区】【类】====================
template<typename T, size_t N>
class circular_list {

public:
    T cache_box[N];             // ← 缓存区
    T abandoned;                // ← 废弃帧
    size_t now_size = 0;        // ← 当前缓存帧数
    size_t last_size = 0;       // ← 上一次缓存帧数
    std::mutex mtx;             // ← 加锁
    std::condition_variable condition;  // ← 条件变量
    
    void*(*abandon_function)(T&) = nullptr;  // ← 丢弃帧的回调函数

    //构造函数
    circular_list(void*(*func)(T&) = nullptr){
        abandon_function = func;      
    }

    //注意：如果传入的是右值则会被清空，如果传入的是左值则不会被清空
    void moveout_push(T item) {
        
        std::lock_guard<std::mutex> lock(mtx);

        abandoned = std::move(cache_box[0]);
        for (size_t i = 0; i < N - 1; ++i)
            cache_box[i] = std::move(cache_box[i + 1]);
        cache_box[N - 1] = std::move(item);  

        if (abandon_function){
            abandon_function(abandoned);
        }
        
        if (now_size < N)
        {
            now_size++; 
        }
        condition.notify_one();

    }

    // 阻塞等待，有新帧才返回（信号量消费）
    T get_new() {
        std::unique_lock<std::mutex> lock(mtx);
        condition.wait(lock, [this]{ return now_size > 0; });

        now_size--;
        return cache_box[N - 1];
    }

    // 非阻塞：没帧立即返回空 T
    T try_get_new() {
        std::lock_guard<std::mutex> lock(mtx);
        if (now_size == 0) return T();

        if (now_size >= last_size)
        {
            last_size = now_size;
            now_size--;
            return cache_box[N - 1];
        }
        return T();
    }

    size_t put_at(size_t idx, T item) 
    {   
        std::lock_guard<std::mutex> lock(mtx);
        if (now_size < N) now_size++;
        cache_box[idx] = std::move(item);  
        return now_size;
    }
    
    T get_at(size_t idx) 
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (now_size > 0) now_size--;
        return cache_box[idx];
    }

    T take_at(size_t idx) 
    {
        std::lock_guard<std::mutex> lock(mtx);

        T item = std::move(cache_box[idx]);
        cache_box[idx] = T();   // ★ 整个槽归零
        if (now_size > 0) now_size--;
        return std::move(item);
    }

    size_t get_now_size() 
    { 
        std::lock_guard<std::mutex> lock(mtx); 
        return now_size; 
    }

};
//====================【线程任务包】【结构体】====================
    //circular_list<_xxx_,number> xxx中number标识最高支持几个摄像头{gts_vpu_package除外}
struct gts_vpu_package
{
    circular_list<gts_vpu_package_detail, 3> vpu_box_list{ex_vpu_result}; //视频+vpu流环形缓存区
    PSQ_gts* gts = NULL;
    PSQ_vpu* vpu = NULL;
    PSQ_strategic* psq_strategic = NULL;

    int video_id = 0;
};

struct rga_package
{
    circular_list<rga_cuerent_package_detail,5> rga_box_list{nullptr}; //rga流环形缓存区
    std::vector<gts_vpu_package*>* gv_current_package = nullptr;
    PSQ_rga* rga = NULL;
    PSQ_strategic* psq_strategic = NULL;
};


struct rknn_yolo_package
{
    PSQ_rknn* rknn = NULL;
    PSQ_YOLOv8* yolov8 = NULL;
    PSQ_strategic* psq_strategic = NULL;
    circular_list<rknn_yolo_package_detail,5> yolo_box_list{nullptr};
    rga_package* rga_current_package = NULL;
};


struct http_package
{
    std::vector<PSQ_http*> http_list;
    PSQ_YOLOv8* yolov8 = NULL;

    PSQ_strategic* psq_strategic = NULL;
    circular_list<http_package_detail,5> http_box_list{nullptr};
    rknn_yolo_package* rkolo_current_package = NULL;

};
//====================【模块】【结构体】====================

struct running_class
{
    PSQ_rknn* rknn;
    std::vector<PSQ_gts*> gts_list;
    std::vector<PSQ_vpu*> vpu_list;
    PSQ_rga* rga;
    PSQ_YOLOv8* yolov8;
    std::vector<PSQ_http*> http_list;
};

struct gts_vpu_current_info_
{
    bool running_flag = false;
    std::vector<pthread_t> current_pthread_id;
    std::vector<gts_vpu_package*> gv_current_package;
};

struct rga_current_info_
{
    bool running_flag = false;
    pthread_t current_pthread_id;
    rga_package* rga_current_package = NULL;
};

struct rknn_yolo_current_info_
{
    bool running_flag = false;
    pthread_t current_pthread_id;
    rknn_yolo_package* rkolo_current_package = NULL;
};

struct http_current_info_
{
    bool running_flag = false;
    pthread_t current_pthread_id;
    http_package* http_current_package = NULL;
};

//====================【线程函数声明】====================

void* thread_func_gts_vpu(void* arg);
void* thread_func_rga(void* arg);
void* thread_func_rknn_yolo(void* arg);
void* thread_func_http(void* arg);

//====================【类】====================
class PSQ_strategic
{
public:


    //==【构造函数】==
    PSQ_strategic(
        int ex_stream_num, 
        std::vector<PSQ_gts*>* gts_list,
        std::vector<PSQ_vpu*>* vpu_list,
        PSQ_rga* rga,
        PSQ_rknn* rknn,
        PSQ_YOLOv8* yolov8,
        std::vector<PSQ_http*>* http_list
    )
    {   
        // 初始化摄像头数量
        stream_num = ex_stream_num;
        // 初始化运行类成员
        run_class.gts_list = *gts_list;
        run_class.vpu_list = *vpu_list;
        run_class.rga = rga;
        run_class.yolov8 = yolov8;
        run_class.rknn = rknn;
        run_class.http_list = *http_list;
    }

    //==【析构函数】==
    ~PSQ_strategic()
    {
        if(observer_class != nullptr)
        {
            delete observer_class;
            observer_class = nullptr;
        }
    }

    //==【gts_vpu流】==
    void proces_gts_vpu()
    {
        //填写每个线程包
        gv_current_info.gv_current_package.resize(stream_num);
        gv_current_info.current_pthread_id.resize(stream_num);

        for(int i = 0; i < stream_num; i++)
        {
            gv_current_info.gv_current_package[i] = new gts_vpu_package();
            gv_current_info.gv_current_package[i]->gts = run_class.gts_list[i];
            gv_current_info.gv_current_package[i]->vpu = run_class.vpu_list[i];
            gv_current_info.gv_current_package[i]->psq_strategic = this;
            gv_current_info.gv_current_package[i]->video_id = i;
            
        }
        gv_current_info.running_flag = true;
        //打开所有摄像头
        for(int i = 0; i < stream_num; i++)
        {
            run_class.gts_list[i]->start();
        }

        //创建线程
        for(int i = 0; i < stream_num; i++)
        {
            int ret  = pthread_create(
                                        &gv_current_info.current_pthread_id[i], 
                                        nullptr, 
                                        thread_func_gts_vpu, 
                                        gv_current_info.gv_current_package[i]
                                    );
        }

    }


    void end_gts_vpu()
    {
        gv_current_info.running_flag = false;
        //关闭所有摄像头
        for(int i = 0; i < stream_num; i++)
        {
            run_class.gts_list[i]->stop();
        }

        //等待线程结束
        for(int i = 0; i < stream_num; i++)
        {
            pthread_join(gv_current_info.current_pthread_id[i], nullptr);
        }

        //释放线程包
        for(int i = 0; i < stream_num; i++)
        {
            delete gv_current_info.gv_current_package[i];
            gv_current_info.gv_current_package[i] = nullptr;
        }
    }

    //==【rga流】==
    void proces_rga()
    {
        //填写1个线程包
        rga_current_info.running_flag = true;
        rga_current_info.rga_current_package = new rga_package();

        rga_current_info.rga_current_package->rga = run_class.rga;
        rga_current_info.rga_current_package->psq_strategic = this;
        rga_current_info.rga_current_package->gv_current_package = &gv_current_info.gv_current_package;
        

        //创建线程

        int ret  = pthread_create(
                                    &rga_current_info.current_pthread_id, 
                                    nullptr, 
                                    thread_func_rga, 
                                    rga_current_info.rga_current_package
                                );
    }
    void end_rga()
    {
        rga_current_info.running_flag = false;

        //等待线程结束
        pthread_join(rga_current_info.current_pthread_id, nullptr);

        //释放线程包
        delete rga_current_info.rga_current_package;
        rga_current_info.rga_current_package = nullptr;
    }


    //==【rknn_yolo流】==
    void proces_rknn_yolo()
    {
        //填写线程包
        rknn_yolo_current_info.running_flag = true;
        rknn_yolo_current_info.rkolo_current_package = new rknn_yolo_package();
        rknn_yolo_current_info.rkolo_current_package->rknn = run_class.rknn;
        rknn_yolo_current_info.rkolo_current_package->yolov8 = run_class.yolov8;
        rknn_yolo_current_info.rkolo_current_package->psq_strategic = this;
        rknn_yolo_current_info.rkolo_current_package->rga_current_package = rga_current_info.rga_current_package;
        
        //创建线程
        int ret  = pthread_create(
                                    &rknn_yolo_current_info.current_pthread_id, 
                                    nullptr, 
                                    thread_func_rknn_yolo, 
                                    rknn_yolo_current_info.rkolo_current_package
                                );
    }

    void end_rknn_yolo()
    {
        rknn_yolo_current_info.running_flag = false;

        //等待线程结束
        pthread_join(rknn_yolo_current_info.current_pthread_id, nullptr);

        //释放线程包
        delete rknn_yolo_current_info.rkolo_current_package;
        rknn_yolo_current_info.rkolo_current_package = nullptr;
    }

    //==【http流】==
    void proces_http()
    {
        //填写线程包
        http_current_info.running_flag = true;
        http_current_info.http_current_package = new http_package();
        http_current_info.http_current_package->http_list = run_class.http_list;
        http_current_info.http_current_package->yolov8 = run_class.yolov8;
        http_current_info.http_current_package->psq_strategic = this;
        http_current_info.http_current_package->rkolo_current_package = rknn_yolo_current_info.rkolo_current_package;
        
        //创建线程
        int ret  = pthread_create(
                                    &http_current_info.current_pthread_id, 
                                    nullptr, 
                                    thread_func_http, 
                                    http_current_info.http_current_package
                                );
    }

    void end_http()
    {
        http_current_info.running_flag = false;

        //等待线程结束
        pthread_join(http_current_info.current_pthread_id, nullptr);

        //释放线程包
        delete http_current_info.http_current_package;
        http_current_info.http_current_package = nullptr;
    }


    void set_debug(bool ex_debug)
    {
        run_class.rknn->debug = ex_debug;
        run_class.yolov8->debug = ex_debug;
        run_class.rga->debug = ex_debug;

        for(int i = 0; i < stream_num; i++)
        {
            run_class.gts_list[i]->debug = ex_debug;
            run_class.vpu_list[i]->debug = ex_debug;
            run_class.http_list[i]->debug = ex_debug;
        }
    }

    //需要在开启各流水线前设置observer_class
    void set_observer(Observer_config config)
    {
        this->observer_config = config;

        if(config.manage_observer)
        {
            if(observer_class == nullptr)
            {
                observer_class = new PSQ_USE_observer(config , this);
                observer_class->pth_out.cache_info.gts_vpu_cache_size.resize(stream_num);

            }else{
                printf("observer已经存在\n");
            }
        }else{
            if(observer_class != nullptr)
            {
                observer_config.manage_observer = false;
                observer_config.internal_rga_USE = false;
                observer_config.internal_vpu_USE = false;
                observer_config.internal_npu_USE = false;

                delete observer_class;
                observer_class = nullptr;

                printf("observer已释放\n");
            }else{
                printf("observer不存在\n");
            }
        }
    }


private:

    PSQ_USE_observer* observer_class = nullptr;
    Observer_config observer_config;

    //摄像头数量
    int stream_num = 0;
    //运行类成员
    running_class run_class;

    //gts_vpu流——线程信息
    gts_vpu_current_info_ gv_current_info;

    //rga流——线程信息
    rga_current_info_ rga_current_info;

    //rknn_yolo流——线程信息
    rknn_yolo_current_info_ rknn_yolo_current_info;

    //http流——线程信息
    http_current_info_ http_current_info;
    

friend void* thread_func_gts_vpu(void* arg);
friend void* thread_func_rga(void* arg);
friend void* thread_func_rknn_yolo(void* arg);

friend void* thread_func_rga_modle2(void* arg);
friend void* thread_func_http(void* arg);

friend void* USE_figure_out(void* arg);

};


//====================【线程函数实现】====================
//gts_vpu线程函数  gtsvpuxiancheng
void* thread_func_gts_vpu(void* arg)
{
    

    gts_vpu_package* package = (gts_vpu_package*)arg;

    //用于存放gts获取到的视频数据
    void* data = nullptr;
    //用于存放gts获取到的视频数据大小
    size_t data_size = 0;


    while(package->psq_strategic->gv_current_info.running_flag)
    {
        int vpu_err = 0;

        gts_vpu_package_detail detail;
        after_vpu_frame_info af_info;

        data_size = package->gts->get_data(&data);
        if (data_size == 0)
        {
            usleep(5000); 
            continue; 
        }

        package->vpu->send_to_vpu(data, data_size);

        if(package->psq_strategic->observer_config.internal_vpu_USE && package->psq_strategic->observer_config.manage_observer)
        {
            struct form_pross_info _form_pross_info;
            std::chrono::time_point<std::chrono::steady_clock> start;
            std::chrono::time_point<std::chrono::steady_clock> end;

            start = std::chrono::steady_clock::now();
            vpu_err = package->vpu->get_data_from_vpu_model_3(&af_info, false);
            end = std::chrono::steady_clock::now();

            std::chrono::nanoseconds time = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
            _form_pross_info.device = VPU;
            _form_pross_info.now_work_time = time;
            _form_pross_info.all_error_num = vpu_err;

            package->psq_strategic->observer_class->sent_to_observer(std::move(_form_pross_info));
            
        }else{
            vpu_err = package->vpu->get_data_from_vpu_model_3(&af_info, true);
        }

        if (vpu_err <= 0) continue;

        detail.af_frame = af_info;
        detail.video_id = package->video_id;

        package->vpu_box_list.moveout_push(std::move(detail));
    }
    return nullptr;
}

//rga线程函数--rgaxiancheng
void* thread_func_rga(void* arg)
{
    rga_package* package = (rga_package*)arg;
    int gv_cur_number = package->psq_strategic->stream_num;

    int    src_fd;
    int    hor_stride;
    int    ver_stride;
    size_t src_size;
    int    video_id;
    MppBuffer mb = NULL;

    size_t dst_size = package->rga->dst_size();
    size_t temp_size = package->rga->tmp_size();

    while (package->psq_strategic->rga_current_info.running_flag)
    {
         bool got_any = false;

        gts_vpu_package_detail gv_detail;
        after_vpu_frame_info   af_info;

        for (int i = 0; i < gv_cur_number; i++)
        {
            gv_detail = package->gv_current_package->at(i)->vpu_box_list.try_get_new();

            if (gv_detail.af_frame.mb == NULL){continue;}

            got_any = true;

            
            af_info   = gv_detail.af_frame;

            video_id   = gv_detail.video_id;
            src_fd     = af_info.buf_fd;
            hor_stride = af_info.hor_stride;
            ver_stride = af_info.ver_stride;
            src_size   = af_info.data_size;

            mb = af_info.mb;
            
            mpp_buffer_inc_ref(af_info.mb);

            //如果observer_config.internal_rga_USE && observer_config.manage_observer为true,说明需要观察者
            if(package->psq_strategic->observer_config.internal_rga_USE && package->psq_strategic->observer_config.manage_observer)
            {
                int err = 0;
                struct form_pross_info _form_pross_info;
                std::chrono::time_point<std::chrono::steady_clock> start;
                std::chrono::time_point<std::chrono::steady_clock> end;

                start = std::chrono::steady_clock::now();
                err = package->rga->cvtcolor(src_fd, hor_stride, ver_stride, src_size);//--
                err += package->rga->resize();                                          //--
                end = std::chrono::steady_clock::now();
                
                std::chrono::nanoseconds time = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
                
                _form_pross_info.device = RGA;
                _form_pross_info.now_work_time = time;
                _form_pross_info.all_error_num = err;

                package->psq_strategic->observer_class->sent_to_observer(std::move(_form_pross_info));

            }else{
                package->rga->cvtcolor(src_fd, hor_stride, ver_stride, src_size);
                package->rga->resize();
            }

            rga_cuerent_package_detail detail;

            detail.video_id = video_id;

            detail.destination.resize(dst_size);
            memcpy(detail.destination.data(), package->rga->dst_va(), dst_size);
            detail.dst_size = dst_size;

            detail.temp.resize(temp_size);
            memcpy(detail.temp.data(), package->rga->tmp_va(), temp_size);
            detail.temp_size = temp_size;
            detail.temp_w = package->rga->tmp_w();
            detail.temp_h = package->rga->tmp_h();

            //detail.rga_flag = true;

            mpp_buffer_put(mb);

            size_t rga_put_size = package->rga_box_list.put_at(i, std::move(detail));

            //容器now_size【rga线程同时记录gts_vpu和rga的now_size -- 从put_at返回值拿，无竞态】
            if(package->psq_strategic->observer_config.manage_observer)
            {
                for (int i = 0; i < gv_cur_number; i++)
                {
                    package->psq_strategic->observer_class->pth_out.cache_info.gts_vpu_cache_size[i] = package->gv_current_package->at(i)->vpu_box_list.get_now_size();
                }

                package->psq_strategic->observer_class->pth_out.cache_info.rga_cache_size = rga_put_size;
            }
        }

        if (!got_any) usleep(5000);

    }
    return nullptr;
}

//rknn_yolo线程函数
void* thread_func_rknn_yolo(void* arg)
{
    rknn_yolo_package* package = (rknn_yolo_package*)arg;

    rga_cuerent_package_detail rga_detail;

    while(package->psq_strategic->rknn_yolo_current_info.running_flag)
    {

         bool got_any = false;

        //从rga环形缓存区拿数据出来
        for(int i = 0; i < package->psq_strategic->stream_num; i++)
        {
            rga_detail = package->rga_current_package->rga_box_list.take_at(i);
            //if (!rga_detail.rga_flag) continue;
            if (rga_detail.destination.empty()) continue;

            got_any = true;

            if(package->psq_strategic->observer_config.internal_npu_USE && package->psq_strategic->observer_config.manage_observer)
            {
                int err = 0;
                struct form_pross_info _form_pross_info;
                std::chrono::time_point<std::chrono::steady_clock> start;
                std::chrono::time_point<std::chrono::steady_clock> end;

                start = std::chrono::steady_clock::now();
                //推理
                err = package->rknn->run_single(rga_detail.destination.data());
                end = std::chrono::steady_clock::now();

                std::chrono::nanoseconds time = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
                
                _form_pross_info.device = NPU;
                _form_pross_info.now_work_time = time;
                _form_pross_info.all_error_num = err;

                package->psq_strategic->observer_class->sent_to_observer(std::move(_form_pross_info));


            }else{
                //推理
                package->rknn->run_single(rga_detail.destination.data());
            }
            //后处理
            package->yolov8->postprocessing();


            //将输出放到环形缓存区
            rknn_yolo_package_detail detail;

            detail.video_id = rga_detail.video_id;
            detail.recognize_boxs = package->yolov8->get_final_vec();

            detail.temp = std::move(rga_detail.temp);
            detail.destination = std::move(rga_detail.destination);

            detail.temp_w = rga_detail.temp_w;
            detail.temp_h = rga_detail.temp_h;

            detail.temp_size = rga_detail.temp_size;
            detail.dst_size = rga_detail.dst_size;
            detail.waiting_task = true;


            package->yolo_box_list.put_at(i, std::move(detail));
        }

        if (!got_any) usleep(5000);

    }
    return nullptr;
}

//http线程函数
void* thread_func_http(void* arg)
{
    http_package* package = (http_package*)arg;

    rknn_yolo_package_detail rknn_yolo_detail;

    while(package->psq_strategic->http_current_info.running_flag)
    {

        bool got_any = false;

        //从yolo环形缓存区拿数据出来
        for(int i = 0; i < package->psq_strategic->stream_num; i++)
        {
            rknn_yolo_detail = package->rkolo_current_package->yolo_box_list.take_at(i);


            //rknn_yolo容器now_size【线程同时记录rknn_yolo的now_size】
            if(package->psq_strategic->observer_config.manage_observer)
            {
                package->psq_strategic->observer_class->pth_out.cache_info.rknn_yolo_cache_size = package->rkolo_current_package->yolo_box_list.get_now_size();
            }

            if(!rknn_yolo_detail.waiting_task)
            {
                continue;
            }

            got_any = true;

            package->yolov8->draw_boxes(
                rknn_yolo_detail.temp.data(), 
                rknn_yolo_detail.temp_w, rknn_yolo_detail.temp_h, 
                std::move(rknn_yolo_detail.recognize_boxs)
            );

            package->http_list[rknn_yolo_detail.video_id]->manage(
                                                            rknn_yolo_detail.temp.data(),
                                                            rknn_yolo_detail.temp_w, rknn_yolo_detail.temp_h
                                                        );
            
        }

        if (!got_any) usleep(5000);
    }
    return nullptr;
}

#endif