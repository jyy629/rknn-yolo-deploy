#include "http_0.0.0.hh"
#include <poll.h>

#define BOUNDARY "--myboundary"
#define FMT_RGB   0
#define FMT_NV12  1

static const char *html_page =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<!DOCTYPE html>\n"
    "<html><head><meta charset='utf-8'>\n"
    "<title>RK3568 USB Camera</title>\n"
    "<style>\n"
    "body{margin:0;background:#000;display:flex;"
    "justify-content:center;align-items:center;"
    "min-height:100vh;}\n"
    "img{max-width:100vw;max-height:100vh;}\n"
    "</style></head>\n"
    "<body><img src='/stream'/></body></html>\n";

static const char *stream_header =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: multipart/x-mixed-replace; boundary=" BOUNDARY "\r\n"
    "Cache-Control: no-cache\r\n"
    "Connection: close\r\n"
    "\r\n";

static bool send_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

//========== manage：主线程推帧（RGB），非阻塞 ==========
void PSQ_http::manage(void* data, int w, int h)
{
    size_t sz = w * h * 3;

    // ★ 先快照源数据到内部缓冲（源可能是 DMA buffer，尽快拷完）
    if (internal_buf.size() != sz)
        internal_buf.resize(sz);
    memcpy(internal_buf.data(), data, sz);

    pthread_mutex_lock(&frame_mutex);

    if (shared_frame.size() != sz)
        shared_frame.resize(sz);
    memcpy(shared_frame.data(), internal_buf.data(), sz);
    frame_w   = w;
    frame_h   = h;
    frame_fmt = FMT_RGB;
    frame_ready_flag = true;

    pthread_cond_signal(&frame_cond);
    pthread_mutex_unlock(&frame_mutex);
}

//========== manage_nv12：主线程推帧（NV12），非阻塞 ==========
void PSQ_http::manage_nv12(void* data, int w, int h, int hor_stride, int ver_stride)
{
    size_t sz = (size_t)w * h * 3 / 2;

    pthread_mutex_lock(&frame_mutex);

    if (shared_frame.size() != sz)
        shared_frame.resize(sz);

    uint8_t *dst = shared_frame.data();
    uint8_t *src = (uint8_t*)data;

    for (int i = 0; i < h; i++) {
        memcpy(dst, src, w);
        dst += w;
        src += hor_stride;
    }

    src = (uint8_t*)data + (size_t)hor_stride * ver_stride;

    int uv_h = h / 2;
    for (int i = 0; i < uv_h; i++) {
        memcpy(dst, src, w);
        dst += w;
        src += hor_stride;
    }

    frame_w   = w;
    frame_h   = h;
    frame_fmt = FMT_NV12;
    frame_ready_flag = true;

    pthread_cond_signal(&frame_cond);
    pthread_mutex_unlock(&frame_mutex);
}

//========== 构造函数 ==========
PSQ_http::PSQ_http(int port)
    : port(port), server_fd(-1), stop(false),
      frame_ready_flag(false), frame_w(0), frame_h(0), frame_fmt(FMT_RGB)
{
    printf("-----------http 初始化中----------------\n");

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return;
    }

    if (listen(server_fd, 5) < 0) {
        perror("listen");
        return;
    }

    pthread_mutex_init(&clients_mutex, NULL);
    pthread_mutex_init(&frame_mutex, NULL);
    pthread_cond_init(&frame_cond, NULL);

    pthread_create(&worker_tid, NULL, worker_thread, this);
    pthread_create(&accept_tid, NULL, accept_thread, this);

    printf("http 初始化完成, 端口=%d!!!!!\n", port);
}

//========== 析构函数 ==========
PSQ_http::~PSQ_http()
{
    printf("~http 析构: 关闭 HTTP 服务...\n");
    stop = true;

    shutdown(server_fd, SHUT_RDWR);
    close(server_fd);

    pthread_join(accept_tid, NULL);

    pthread_mutex_lock(&frame_mutex);
    frame_ready_flag = true;
    pthread_cond_signal(&frame_cond);
    pthread_mutex_unlock(&frame_mutex);
    pthread_join(worker_tid, NULL);

    pthread_mutex_lock(&clients_mutex);
    for (int fd : clients) close(fd);
    clients.clear();
    pthread_mutex_unlock(&clients_mutex);

    pthread_mutex_destroy(&clients_mutex);
    pthread_mutex_destroy(&frame_mutex);
    pthread_cond_destroy(&frame_cond);
    printf("~http 析构完成\n");
}

//========== accept 线程 ==========
void* PSQ_http::accept_thread(void* arg)
{
    PSQ_http* http = (PSQ_http*)arg;

    while (!http->stop) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(http->server_fd,
                               (struct sockaddr *)&client_addr,
                               &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (http->stop) break;
            perror("accept");
            continue;
        }

        printf("HTTP 连接: %s:%d\n",
               inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port));

        char buf[4096];

        struct pollfd pfd;
        pfd.fd = client_fd;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 1000);
        if (pr <= 0 || http->stop) {
            close(client_fd);
            continue;
        }

        ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            close(client_fd);
            continue;
        }
        buf[n] = '\0';

        if (strncmp(buf, "GET /stream", 11) == 0) {
            send_all(client_fd, stream_header, strlen(stream_header));

            pthread_mutex_lock(&http->clients_mutex);
            http->clients.push_back(client_fd);
            pthread_mutex_unlock(&http->clients_mutex);

            printf("  客户端订阅流, 当前共 %zu 个\n", http->clients.size());
        } else {
            send_all(client_fd, html_page, strlen(html_page));
            close(client_fd);
        }
    }
    return NULL;
}

//========== worker 线程：阻塞等待 manage() 唤醒 ==========
void* PSQ_http::worker_thread(void* arg)
{
    PSQ_http* http = (PSQ_http*)arg;

    while (!http->stop) {
        // 阻塞等待 manage() 推帧
        pthread_mutex_lock(&http->frame_mutex);
        while (!http->frame_ready_flag && !http->stop) {
            pthread_cond_wait(&http->frame_cond, &http->frame_mutex);
        }
        if (http->stop) {
            pthread_mutex_unlock(&http->frame_mutex);
            break;
        }

        // 从共享缓冲拷出，尽快释放锁
        int w   = http->frame_w;
        int h   = http->frame_h;
        int fmt = http->frame_fmt;
        size_t sz = http->shared_frame.size();
        std::vector<unsigned char> local_frame(sz);
        memcpy(local_frame.data(), http->shared_frame.data(), sz);
        http->frame_ready_flag = false;
        pthread_mutex_unlock(&http->frame_mutex);

        // 编码+发送（锁外，不阻塞主线程推下一帧）
        cv::Mat bgr;
        if (fmt == FMT_NV12) {
            cv::Mat nv12(h + h / 2, w, CV_8UC1, local_frame.data());
            cv::cvtColor(nv12, bgr, cv::COLOR_YUV2BGR_NV12);
        } else {
            cv::Mat rgb(h, w, CV_8UC3, local_frame.data());
            cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
        }

        std::vector<uchar> jpg;
        cv::imencode(".jpg", bgr, jpg,
                     std::vector<int>{cv::IMWRITE_JPEG_QUALITY, 80});

        char hdr[256];
        int hdr_len = snprintf(hdr, sizeof(hdr),
            "\r\n--" BOUNDARY "\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %zu\r\n"
            "\r\n", jpg.size());

        pthread_mutex_lock(&http->clients_mutex);
        for (auto it = http->clients.begin(); it != http->clients.end(); ) {
            bool ok = send_all(*it, hdr, hdr_len) &&
                      send_all(*it, jpg.data(), jpg.size());
            if (!ok) {
                close(*it);
                it = http->clients.erase(it);
                printf("  客户端断开, 剩余 %zu 个\n", http->clients.size());
            } else {
                ++it;
            }
        }
        pthread_mutex_unlock(&http->clients_mutex);
    }
    return NULL;
}