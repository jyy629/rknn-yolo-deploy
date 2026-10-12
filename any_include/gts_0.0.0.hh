#ifndef GTS_0_0_0_HH
#define GTS_0_0_0_HH

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <cstdint>
#include <memory>
#include <errno.h>
#include <pthread.h>



class PSQ_gts;

struct gts_info
{
    const char * url;
    GstElement * pipeline;
    GstElement * rtspsrc;
    GstElement * depay;
    GstElement * parse;
    GstElement * capsfilter;
    GstElement * appsink;
};

static void on_pad_added(GstElement *src, GstPad *pad, gpointer data)
{
    GstElement *depay = GST_ELEMENT(data);
    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    if (!caps) return;
    gchar *str = gst_caps_to_string(caps);
    printf("  pad caps: %s\n", str);
    int is_video = (strstr(str, "encoding-name=(string)H265") ||
                    strstr(str, "encoding-name=(string)H264"));
    g_free(str); gst_caps_unref(caps);
    if (is_video) {
        GstPad *sink = gst_element_get_static_pad(depay, "sink");
        if (gst_pad_link(pad, sink) == GST_PAD_LINK_OK)
            printf("  -> 已连接视频流\n");
        gst_object_unref(sink);
    }
}

static GstFlowReturn on_new_sample_from_sink(GstAppSink *sink, gpointer data);



class PSQ_gts
{
public:

    bool debug = false;


    friend GstFlowReturn on_new_sample_from_sink(GstAppSink *sink, gpointer data);
    friend void on_pad_added(GstElement *src, GstPad *pad, gpointer data);

    PSQ_gts(const char * url)
    {   
        pthread_mutex_init(&mutex, nullptr);
        this->info.url = url;

        gst_init(nullptr, nullptr);
        // 1. 创建元素
        this->info.rtspsrc  = gst_element_factory_make("rtspsrc",     "rtsp-source");
        this->info.depay   = gst_element_factory_make("rtph265depay", "depay");
        this->info.parse   = gst_element_factory_make("h265parse",    "parser");
        this->info.capsfilter   = gst_element_factory_make("capsfilter", "filter");
        this->info.appsink = gst_element_factory_make("appsink", "appsink");
        if (!info.rtspsrc || !info.depay || !info.parse || !info.capsfilter || !info.appsink) {
            g_printerr("元素创建失败。请检查插件是否已安装。\n");
        }
        // 2. 创建管道
        this->info.pipeline = gst_pipeline_new("rtsp-pipeline");

        // 3. 设置元素属性
        g_object_set(
            this->info.rtspsrc,
            "location", info.url,
            "latency",  200,      /* 抖动缓冲 200ms，降低延迟 */
            "timeout",  5000000,  /* 5 秒无数据则超时（微秒） */
            NULL
        );

        g_object_set(this->info.parse, "config-interval", -1, nullptr);

        GstCaps *caps = gst_caps_new_simple("video/x-h265",
                                "stream-format", G_TYPE_STRING, "byte-stream",
                                "alignment",     G_TYPE_STRING, "au",
                                NULL);
        g_object_set(this->info.capsfilter, "caps", caps, nullptr); 
        gst_caps_unref(caps);

        g_object_set(this->info.appsink, "sync", false, "emit-signals", TRUE,
                     "max-buffers", 5, "drop", TRUE, nullptr);

        // 4. 添加元素到管道
        gst_bin_add_many(GST_BIN(info.pipeline),info.rtspsrc, info.depay, info.parse, info.capsfilter, info.appsink, NULL);
        // 5. 连接元素
        gst_element_link_many(info.depay, info.parse, info.capsfilter, info.appsink, nullptr);

        g_signal_connect(info.rtspsrc, "pad-added", G_CALLBACK(on_pad_added), info.depay);

        g_signal_connect(info.appsink, "new-sample", G_CALLBACK(on_new_sample_from_sink), this);

    }



    ~PSQ_gts() { stop(); }

    // 返回 H.265 码流大小，*va 指向内部分配的内存（智能指针管理，下次调用/析构自动释放）
    size_t get_data(void **va) 
    {
        pthread_mutex_lock(&mutex);

        size_t size = 0;

        if (this->buffer) {
            GstMapInfo map;
            gst_buffer_map(this->buffer, &map, GST_MAP_READ);

            if (*va == nullptr)
            {
                *va = malloc(4 * 1024 * 1024);
            }
            memcpy(*va, map.data, map.size); 

            size = map.size;


            gst_buffer_unmap(this->buffer, &map);
            gst_sample_unref(this->sample);
            this->sample = nullptr;
            this->buffer = nullptr; 

        }

        pthread_mutex_unlock(&mutex);
        return size;
    }

    void release_data(void **va)
    {
        if(*va != nullptr)
        {
            free(*va);
            *va  = nullptr;
        }
    }


    void start()
    {
        gst_element_set_state(info.pipeline, GST_STATE_PLAYING);
    }

    void stop()
    {
        gst_element_set_state(info.pipeline, GST_STATE_NULL);
    }


private:

    gts_info info;

    GstSample *sample     = nullptr;
    GstBuffer *buffer     = nullptr;


    pthread_mutex_t mutex;







};



static GstFlowReturn on_new_sample_from_sink(GstAppSink *sink, gpointer data) {

    PSQ_gts *self = (PSQ_gts *)data;

    pthread_mutex_lock(&self->mutex);
    if (self->sample) gst_sample_unref(self->sample);
    self->sample = gst_app_sink_pull_sample(sink);
    self->buffer = gst_sample_get_buffer(self->sample);
    pthread_mutex_unlock(&self->mutex);

    return GST_FLOW_OK;
}



#endif