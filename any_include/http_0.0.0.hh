#ifndef HTTP_HH
#define HTTP_HH

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <vector>
#include <atomic>
#include <opencv2/opencv.hpp>
#include <opencv2/imgproc.hpp>

class PSQ_http
{
public:

    bool debug = false;


    PSQ_http(int port);
    ~PSQ_http();

    void manage(void* data, int w, int h);
    void manage_nv12(void* data, int w, int h, int hor_stride, int ver_stride);

private:
    int server_fd;
    int port;
    pthread_t worker_tid;
    pthread_t accept_tid;
    std::vector<int> clients;
    pthread_mutex_t clients_mutex;

    pthread_mutex_t frame_mutex;
    pthread_cond_t  frame_cond;
    bool frame_ready_flag;
    int frame_w, frame_h;
    int frame_fmt;  // 0=RGB 1=NV12
    std::vector<unsigned char> shared_frame;
    std::vector<unsigned char> internal_buf;

    std::atomic<bool> stop;

    static void* accept_thread(void* arg);
    static void* worker_thread(void* arg);
};

#endif