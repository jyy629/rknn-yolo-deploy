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
#include "rga.hh"

class PSQ_http
{
public:
    PSQ_http(int port, PSQ_rga& rga);
    ~PSQ_http();

    void frame_ready();

private:
    PSQ_rga& class_rga;
    int server_fd;
    int port;
    pthread_t worker_tid;
    pthread_t accept_tid;
    std::vector<int> clients;
    pthread_mutex_t clients_mutex;
    pthread_mutex_t frame_mutex;
    pthread_cond_t  frame_cond;
    bool frame_ready_flag;
    std::atomic<bool> stop;

    static void* accept_thread(void* arg);
    static void* worker_thread(void* arg);
};

#endif