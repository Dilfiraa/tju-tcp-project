#include "tju_tcp.h"

/*
创建 TCP socket 
初始化对应的结构体
设置初始状态为 CLOSED
*/
tju_tcp_t *tju_socket()
{
    tju_tcp_t *sock = (tju_tcp_t *)malloc(sizeof(tju_tcp_t));
    sock->state = CLOSED;

    pthread_mutex_init(&(sock->send_lock), NULL);
    sock->sending_buf = NULL;
    sock->sending_buf_send_len = 0;
    sock->sending_len = 0;

    pthread_mutex_init(&(sock->recv_lock), NULL);
    sock->received_buf = NULL;
    sock->received_len = 0;

    if (pthread_cond_init(&sock->wait_cond, NULL) != 0)
    {
        perror("ERROR condition variable not set\n");
        exit(-1);
    }

    // 初始化发送窗口
    sock->window.wnd_send = malloc(sizeof(sender_window_t));
    sock->window.wnd_send->base = 1;
    sock->window.wnd_send->nextseq = 1;
    sock->window.wnd_send->window_size = TCP_SENDWN_SIZE;
    pthread_mutex_init(&(sock->window.wnd_send->ack_cnt_lock), NULL);
    sock->window.wnd_send->ack_cnt = 0;
    sock->window.wnd_send->cwnd = MAX_DLEN;
    sock->window.wnd_send->rwnd = TCP_BUF_SIZE;
    sock->window.wnd_send->congestion_status = SLOW_START;
    sock->window.wnd_send->ssthresh = 10 * MAX_DLEN;
    sock->window.wnd_send->timeout.it_value.tv_sec = 0;
    sock->window.wnd_send->timeout.it_value.tv_usec = 800000;
    sock->window.wnd_send->timeout.it_interval.tv_sec = 0;
    sock->window.wnd_send->timeout.it_interval.tv_usec = 0;

    sock->window.wnd_send->dev_rtt = 0;
    sock->window.wnd_send->estmated_rtt = 0;
    sock->window.wnd_send->is_estimating_rtt = false;

    // 初始化接收窗口
    sock->window.wnd_recv = malloc(sizeof(receiver_window_t));
    sock->window.wnd_recv->expect_seq = 1;

    // 初始化队列
    sock->incomplete_conn_queue = NULL;
    sock->complete_conn_queue = NULL;

    // 重传状态
    sock->is_retransing = false;

    return sock;
}

void timeout_handler(int signo)
{
    printf("TIME OUT！！！！\n");
    RETRANS = 1;
    TIMEOUT_FLAG = 1;
    return;
}

void *retrans_thread(void *arg)
{
    int hashval = *((int *)arg);
    tju_tcp_t *sock = established_socks[hashval];

    while (1)
    {
        if (RETRANS)
        {

            while (pthread_mutex_lock(&(sock->send_lock)) != 0)
                ; // 给发送缓冲区加锁

            printf("进入重传函数\n");

            sock->is_retransing = true;

            if (TIMEOUT_FLAG)
            {
                sock->window.wnd_send->ssthresh = sock->window.wnd_send->cwnd / 2;
                sock->window.wnd_send->cwnd = MAX_DLEN;
                while (pthread_mutex_lock(&(sock->window.wnd_send->ack_cnt_lock)) != 0)
                    ;
                sock->window.wnd_send->ack_cnt = 0;
                pthread_mutex_unlock(&(sock->window.wnd_send->ack_cnt_lock));
                sock->window.wnd_send->congestion_status = SLOW_START;
            }

            uint32_t retrans_base = 0;
            uint32_t retransed_size = 0;
            uint32_t wnd_base = sock->window.wnd_send->base;
            uint32_t wnd_next = sock->window.wnd_send->nextseq;
            uint32_t wnd_retrans_size = wnd_next - wnd_base;
            uint16_t rwnd = sock->window.wnd_send->rwnd;
            uint16_t cwnd = sock->window.wnd_send->cwnd;

            // printf("发送窗口 base=%d, nextseq=%d\n", sock->window.wnd_send->base, sock->window.wnd_send->nextseq);
            // printf("发送缓冲区 sending_len=%d, sending_buf_send_len=%d\n", sock->sending_len, sock->sending_buf_send_len);

            // printf("-----需发送的数据 小于 发送窗口剩余的大小-----\n");
            // 需发送的数据大于MAX_DLEN
            while (wnd_retrans_size > MAX_DLEN)
            {

                char *msg;
                uint32_t seq = wnd_base + retransed_size;
                uint16_t plen = DEFAULT_HEADER_LEN + MAX_DLEN;

                char *data = malloc(MAX_DLEN);
                memcpy(data, sock->sending_buf + retransed_size, MAX_DLEN);
                msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, seq, 0,
                                        DEFAULT_HEADER_LEN, plen, NO_FLAG, 1, 0, data, MAX_DLEN);

                sendToLayer3(msg, plen);

                if (retrans_base == retransed_size)
                {
                    startTimer(sock);
                }

                retransed_size += MAX_DLEN;
                wnd_retrans_size -= MAX_DLEN;

                printf("重传 1375 大小的报文 seq = %d\n", seq);
            }

            char *msg;
            uint32_t seq = wnd_base + retransed_size;
            uint32_t len = wnd_retrans_size;
            uint16_t plen = DEFAULT_HEADER_LEN + len;

            if (len != 0)
            {
                char *data = malloc(len);
                memcpy(data, sock->sending_buf + retransed_size, len);
                msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, seq, 0,
                                        DEFAULT_HEADER_LEN, plen, NO_FLAG, 1, 0, data, len);

                sendToLayer3(msg, plen);
                if (retrans_base == retransed_size)
                {
                    startTimer(sock);
                }

                retransed_size += len;
                wnd_retrans_size -= len;

                printf("重传 %d 大小的报文 seq = %d\n", len, seq);
            }

            RETRANS = 0;
            TIMEOUT_FLAG = 0;
            sock->is_retransing = false;

            pthread_mutex_unlock(&(sock->send_lock)); // 解锁
        }
    }
}

void startTimer(tju_tcp_t *sock)
{
    struct itimerval tick;
    RETRANS = 0;
    signal(SIGALRM, timeout_handler);
    memset(&tick, 0, sizeof(tick));

    memcpy(&tick, &sock->window.wnd_send->timeout, sizeof(tick));

    if (setitimer(ITIMER_REAL, &tick, NULL) < 0)
        printf("Set timer failed!\n");

    printf("START TIMER\n");

    return;
}

void stopTimer(void)
{
    struct itimerval value;
    value.it_value.tv_sec = 0;
    value.it_value.tv_usec = 0;
    value.it_interval.tv_sec = 0;
    value.it_interval.tv_usec = 0;
    printf("STOP TIMER\n");
    setitimer(ITIMER_REAL, &value, NULL);

    return;
}

/*
绑定监听的地址 包括ip和端口
*/
int tju_bind(tju_tcp_t *sock, tju_sock_addr bind_addr)
{
    sock->bind_addr = bind_addr;
    return 0;
}

/*
被动打开 监听bind的地址和端口
设置socket的状态为LISTEN
注册该socket到内核的监听socket哈希表
*/
int tju_listen(tju_tcp_t *sock)
{
    sock->state = LISTEN;
    sock->incomplete_conn_queue = createQueue();
    sock->complete_conn_queue = createQueue();
    int hashval = cal_hash(sock->bind_addr.ip, sock->bind_addr.port, 0, 0);
    listen_socks[hashval] = sock;
    return 0;
}

/*
接受连接 
返回与客户端通信用的socket
这里返回的socket一定是已经完成3次握手建立了连接的socket
因为只要该函数返回, 用户就可以马上使用该socket进行send和recv
*/
tju_tcp_t *tju_accept(tju_tcp_t *listen_sock)
{
    // 在handle函数中解决
    while (!listen_sock->complete_conn_queue->queue_size)
        ;

    tju_tcp_t *new_conn;
    new_conn = deQueue(listen_sock->complete_conn_queue);

    tju_sock_addr local_addr, remote_addr;

    local_addr.ip = new_conn->established_local_addr.ip;
    local_addr.port = new_conn->established_local_addr.port;
    remote_addr.ip = new_conn->established_remote_addr.ip;
    remote_addr.port = new_conn->established_remote_addr.port;

    // 将新的conn放到内核建立连接的socket哈希表中
    int hashval = cal_hash(local_addr.ip, local_addr.port, remote_addr.ip, remote_addr.port);
    established_socks[hashval] = new_conn;

    // 如果new_conn的创建过程放到了tju_handle_packet中 那么accept怎么拿到这个new_conn呢
    // 在linux中 每个listen socket都维护一个已经完成连接的socket队列
    // 每次调用accept 实际上就是取出这个队列中的一个元素
    // 队列为空,则阻塞

    pthread_t sending_thread_id = 1004;
    void *sending_thread_arg = malloc(sizeof(&hashval));
    memcpy(sending_thread_arg, &hashval, sizeof(&hashval));
    int rst1 = pthread_create(&sending_thread_id, NULL, sending_thread, sending_thread_arg);
    if (rst1 < 0)
    {
        printf("ERROR open sending thread \n");
        exit(-1);
    }

    pthread_t retrans_thread_id = 1005;
    void *retrans_thread_arg = malloc(sizeof(&hashval));
    memcpy(retrans_thread_arg, &hashval, sizeof(&hashval));
    int rst2 = pthread_create(&retrans_thread_id, NULL, retrans_thread, retrans_thread_arg);
    if (rst2 < 0)
    {
        printf("ERROR open retrans thread \n");
        exit(-1);
    }

    printf("tju_accept：三次握手完成！\n");

    return new_conn;
}

/*
连接到服务端
该函数以一个socket为参数
调用函数前, 该socket还未建立连接
函数正常返回后, 该socket一定是已经完成了3次握手, 建立了连接
因为只要该函数返回, 用户就可以马上使用该socket进行send和recv
*/
int tju_connect(tju_tcp_t *sock, tju_sock_addr target_addr)
{

    tju_sock_addr local_addr;
    local_addr.ip = inet_network("10.0.0.2");
    local_addr.port = 5678; //连接方进行connect连接的时候 内核中是随机分配一个可用的端口
    sock->established_local_addr = local_addr;
    sock->established_remote_addr = target_addr;

    // 下面是补充的部分
    uint32_t seq = CLIENT_CONN_SEQ;
    tju_packet_t *syn_packet = create_packet(local_addr.port, target_addr.port, seq, 0,
                                             DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, SYN_FLAG_MASK, 1, 0, NULL, 0);
    char *msg1 = packet_to_buf(syn_packet);
    sendToLayer3(msg1, DEFAULT_HEADER_LEN);
    printf("发送SYN报文\n");

    sock->state = SYN_SENT;
    // 将建立了连接的socket放入内核 已建立连接哈希表中
    int hashval = cal_hash(local_addr.ip, local_addr.port, target_addr.ip, target_addr.port);
    established_socks[hashval] = sock;

    // 这里也不能直接建立连接 需要经过三次握手
    // 实际在linux中 connect调用后 会进入一个while循环
    // 循环跳出的条件是socket的状态变为ESTABLISHED 表面看上去就是 正在连接中 阻塞
    // 而状态的改变在别的地方进行 在我们这就是tju_handle_packet

    while (sock->state != ESTABLISHED)
        ;

    pthread_t sending_thread_id = 1002;
    void *sending_thread_arg = malloc(sizeof(&hashval));
    memcpy(sending_thread_arg, &hashval, sizeof(&hashval));
    int rst1 = pthread_create(&sending_thread_id, NULL, sending_thread, sending_thread_arg);
    if (rst1 < 0)
    {
        printf("ERROR open send thread \n");
        exit(-1);
    }

    pthread_t retrans_thread_id = 1003;
    void *retrans_thread_arg = malloc(sizeof(&hashval));
    memcpy(retrans_thread_arg, &hashval, sizeof(&hashval));
    int rst2 = pthread_create(&retrans_thread_id, NULL, retrans_thread, retrans_thread_arg);
    if (rst2 < 0)
    {
        printf("ERROR open send thread \n");
        exit(-1);
    }

    printf("tju_connet：三次握手完成！\n");

    return 0;
}

int tju_send(tju_tcp_t *sock, const void *buffer, int len)
{
    // 这里当然不能直接简单地调用sendToLayer3
    char *data = malloc(len);
    memcpy(data, buffer, len);

    tju_buffered(sock, data, len);

    for (int i = 0; i < 8000; i++)
        ;

    return 0;
}

int tju_buffered(tju_tcp_t *sock, char *data, int len)
{
    int print = 1;
    // 如果缓冲区已满 则阻塞等待
    while (TCP_BUF_SIZE - sock->sending_len < len)
    {
        if (print)
        {
            printf("缓冲区已满 阻塞等待\n");
            print = 0;
        }
    }

    // 把收到的数据放到发送缓冲区
    while (pthread_mutex_lock(&(sock->send_lock)) != 0)
        ; // 加锁

    if (sock->sending_buf == NULL)
    {
        sock->sending_buf = malloc(len);
    }
    else
    {
        sock->sending_buf = realloc(sock->sending_buf, sock->sending_len + len);
    }

    memcpy(sock->sending_buf + sock->sending_len, data, len);
    sock->sending_len += len;

    // printf("数据已存入发送缓冲区\n");
    // printf("缓冲区大小：%d\n", sock->sending_len);

    pthread_mutex_unlock(&(sock->send_lock)); // 解锁

    return 0;
}

void *sending_thread(void *arg)
{

    printf("进入发送线程\n");

    int hashval = *((int *)arg);
    tju_tcp_t *sock = established_socks[hashval];

    while (1)
    {

        // 发送缓冲区中还有没有发送过的数据 & 没有在重传 & 发送窗口有剩余序列号
        if (sock->sending_buf_send_len < sock->sending_len && !sock->is_retransing &&
            sock->window.wnd_send->nextseq < sock->window.wnd_send->base + sock->window.wnd_send->window_size)
        {
            while (pthread_mutex_lock(&(sock->send_lock)) != 0)
                ; // 给发送缓冲区加锁

            uint32_t wnd_base = sock->window.wnd_send->base;
            uint32_t wnd_size = sock->window.wnd_send->window_size;
            uint32_t wnd_next = sock->window.wnd_send->nextseq;

            uint32_t buf_len = sock->sending_len;
            uint32_t buf_send_len = sock->sending_buf_send_len;
            uint16_t rwnd = sock->window.wnd_send->rwnd;
            uint16_t cwnd = sock->window.wnd_send->cwnd;

            // printf("发送窗口 base=%d, nextseq=%d\n", sock->window.wnd_send->base, sock->window.wnd_send->nextseq);
            // printf("发送缓冲区 sending_len=%d, sending_buf_send_len=%d\n", sock->sending_len, sock->sending_buf_send_len);

            // 需发送的数据 小于 发送窗口剩余的大小
            if (buf_len - buf_send_len <= wnd_base + wnd_size - wnd_next)
            {
                // printf("-----需发送的数据 小于 发送窗口剩余的大小-----\n");
                // 需发送的数据大于MAX_DLEN
                while (buf_len - buf_send_len > MAX_DLEN && wnd_next + MAX_DLEN - wnd_base <= min(cwnd, rwnd))
                {

                    char *msg;
                    uint32_t seq = wnd_next;
                    uint16_t plen = DEFAULT_HEADER_LEN + MAX_DLEN;

                    char *data = malloc(MAX_DLEN);
                    memcpy(data, sock->sending_buf + buf_send_len, MAX_DLEN);
                    tju_packet_t *pkt = create_packet(sock->established_local_addr.port, sock->established_remote_addr.port, seq, 0,
                                                      DEFAULT_HEADER_LEN, plen, NO_FLAG, 1, 0, data, MAX_DLEN);

                    msg = packet_to_buf(pkt);
                    sendToLayer3(msg, plen);
                    printf("发送 %d 字节大小的报文 seq = %d\n", MAX_DLEN, seq);

                    if(sock->window.wnd_send->is_estimating_rtt == false)
                    {
                        sock->window.wnd_send->is_estimating_rtt = true;
                        gettimeofday(&sock->window.wnd_send->send_time, NULL);
                        sock->window.wnd_send->rtt_expect_ack = seq + MAX_DLEN;
                    }

                    // 如果发送窗口的base和nextseq一样 说明是窗口的第一个 为其启动计时器
                    if (wnd_base == wnd_next)
                    {
                        startTimer(sock);
                    }

                    wnd_next += MAX_DLEN;
                    buf_send_len += MAX_DLEN;
                }

                char *msg;
                uint32_t seq = wnd_next;
                uint32_t len = buf_len - buf_send_len;
                uint16_t plen = DEFAULT_HEADER_LEN + len;

                if (wnd_next + len - wnd_base <= min(cwnd, rwnd))
                {
                    char *data = malloc(len);
                    memcpy(data, sock->sending_buf + buf_send_len, len);
                    tju_packet_t *pkt = create_packet(sock->established_local_addr.port, sock->established_remote_addr.port, seq, 0,
                                                      DEFAULT_HEADER_LEN, plen, NO_FLAG, 1, 0, data, len);
                    msg = packet_to_buf(pkt);
                    sendToLayer3(msg, plen);
                    printf("发送 %d 大小的报文 seq = %d\n", len, seq);
                    
                    if(sock->window.wnd_send->is_estimating_rtt == false)
                    {
                        sock->window.wnd_send->is_estimating_rtt = true;
                        gettimeofday(&sock->window.wnd_send->send_time, NULL);
                        sock->window.wnd_send->rtt_expect_ack = seq + len;
                    }

                    if (wnd_base == wnd_next)
                    {
                        startTimer(sock);
                    }

                    wnd_next += len;
                    buf_send_len += len;
                }

                sock->window.wnd_send->nextseq = wnd_next;
                sock->sending_buf_send_len = buf_send_len;

                pthread_mutex_unlock(&(sock->send_lock)); // 解锁
            }

            // 需发送的数据大于发送窗口剩余的大小
            if (buf_len - buf_send_len > wnd_base + wnd_size - wnd_next)
            {
                // printf("-----需发送的数据 大于 发送窗口剩余的大小-----\n");
                // 需发送窗口可用的序列号大于MAX_DLEN
                while (wnd_base + wnd_size - wnd_next > MAX_DLEN && wnd_next + MAX_DLEN - wnd_base <= min(cwnd, rwnd))
                {

                    char *msg;
                    uint32_t seq = wnd_next;
                    uint16_t plen = DEFAULT_HEADER_LEN + MAX_DLEN;
                    char *data = malloc(MAX_DLEN);
                    memcpy(data, sock->sending_buf + buf_send_len, MAX_DLEN);
                    msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, seq, 0,
                                            DEFAULT_HEADER_LEN, plen, NO_FLAG, 1, 0, data, MAX_DLEN);
                    sendToLayer3(msg, plen);

                    if (wnd_base == wnd_next)
                    {
                        startTimer(sock);

                    }

                    wnd_next += MAX_DLEN;
                    buf_send_len += MAX_DLEN;
                    printf("发送 %d 字节大小的报文 seq = %d\n", MAX_DLEN, seq);
                }

                char *msg;
                uint32_t seq = wnd_next;
                uint32_t len = wnd_base + wnd_size - wnd_next;
                uint16_t plen = DEFAULT_HEADER_LEN + len;

                if (wnd_next + len - wnd_base <= min(cwnd, rwnd))
                {
                    char *data = malloc(len);
                    memcpy(data, sock->sending_buf + buf_send_len, len);
                    msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, seq, 0,
                                            DEFAULT_HEADER_LEN, plen, NO_FLAG, 1, 0, data, len);
                    sendToLayer3(msg, plen);
                    printf("发送 %d 大小的报文 seq = %d\n", len, seq);

                    if (wnd_base == wnd_next)
                    {
                        startTimer(sock);
                    }

                    wnd_next += len;
                    buf_send_len += len;
                }

                sock->window.wnd_send->nextseq = wnd_next;
                sock->sending_buf_send_len = buf_send_len;

                pthread_mutex_unlock(&(sock->send_lock)); // 解锁
            }
        }
    }
}

int tju_recv(tju_tcp_t *sock, void *buffer, int len)
{
    while (sock->received_len <= 0)
        ;

    while (pthread_mutex_lock(&(sock->recv_lock)) != 0)
        ; // 加锁

    int read_len = 0;
    if (sock->received_len >= len)
    { // 从中读取len长度的数据
        read_len = len;
    }
    else
    {
        read_len = sock->received_len; // 读取sock->received_len长度的数据(全读出来)
    }

    memcpy(buffer, sock->received_buf, read_len);

    if (read_len < sock->received_len)
    { // 还剩下一些
        char *new_buf = malloc(sock->received_len - read_len);
        memcpy(new_buf, sock->received_buf + read_len, sock->received_len - read_len);
        free(sock->received_buf);
        sock->received_len -= read_len;
        sock->received_buf = new_buf;
    }
    else
    {
        free(sock->received_buf);
        sock->received_buf = NULL;
        sock->received_len = 0;
    }
    pthread_mutex_unlock(&(sock->recv_lock)); // 解锁

    return 0;
}

int TimeoutInterval(tju_tcp_t *sock)
{
    struct timeval send_time = sock->window.wnd_send->send_time;
    struct timeval local_time;
    gettimeofday(&local_time, NULL);
    // printf("------------------------------- TimeoutInterval -------------------------------\n");
    
    // printf("发送pkt 获取的秒时间 = %ld  获取的微秒时间 = %ld\n", send_time.tv_sec, send_time.tv_usec);
    // printf("收到ack 获取的秒时间 = %ld  获取的微秒时间 = %ld\n", local_time.tv_sec, local_time.tv_usec);

    long sampleRTT = (local_time.tv_sec - send_time.tv_sec) * 1000000 + (local_time.tv_usec - send_time.tv_usec);
    printf("sampleRTT = %ld \n", sampleRTT);

    sock->window.wnd_send->estmated_rtt = 0.875 * sock->window.wnd_send->estmated_rtt + 0.125 * sampleRTT;

    int abs;

    if (sampleRTT >= sock->window.wnd_send->estmated_rtt)
    {
        abs = sampleRTT - sock->window.wnd_send->estmated_rtt;
    }
    else
    {
        abs = sock->window.wnd_send->estmated_rtt - sampleRTT;
    }

    sock->window.wnd_send->dev_rtt = 0.75 * sock->window.wnd_send->dev_rtt + 0.25 * abs;

    sock->window.wnd_send->timeout.it_value.tv_usec = sock->window.wnd_send->estmated_rtt + 4 * sock->window.wnd_send->dev_rtt;

    // printf("TimeOut = %ld \n", sock->window.wnd_send->timeout.it_value.tv_usec);
    // printf("-------------------------------------------------------------------------------\n");

    return 0;
}

int tju_handle_packet(tju_tcp_t *sock, char *pkt)
{
    uint8_t pkt_flag = get_flags(pkt);
    uint32_t pkt_seq = get_seq(pkt);
    uint32_t pkt_ack = get_ack(pkt);

    /* 
        如果哈希表返回的socket为LITEN状态
        (只有服务端的socket才会进入LISTEN状态 因此返回的是服务端的socket) 
        说明是在Lhash中找到的
        也意味着该socket还处在三次握手阶段(并且是服务端)
        因此 它只对两种报文做出反应SYN、ACK
    */
    if (sock->state == LISTEN)
    {
        // 如果收到的是SYN报文 这说明还在握手的第一阶段
        if (pkt_flag == SYN_FLAG_MASK)
        {
            printf("收到客户端的syn报文\n");
            // 创建新的socket
            tju_tcp_t *new_conn = (tju_tcp_t *)malloc(sizeof(tju_tcp_t));
            memcpy(new_conn, sock, sizeof(tju_tcp_t));

            // 从报客户端发来的SYN报文中拿到对端的IP和PORT
            tju_sock_addr local_addr, remote_addr;
            remote_addr.ip = inet_network("10.0.0.2"); //具体的IP地址
            remote_addr.port = get_src(pkt);           //端口

            local_addr.ip = sock->bind_addr.ip;     //具体的IP地址
            local_addr.port = sock->bind_addr.port; //端口

            new_conn->established_local_addr = local_addr;
            new_conn->established_remote_addr = remote_addr;

            // 将连接socket的状态改为SYN_RECV 并存入LISTEN状态的socket的半连接队列中
            new_conn->state = SYN_RECV;
            enQueue(sock->incomplete_conn_queue, new_conn);

            // 向客户端发送SYN_ACK
            uint32_t seq = SERVER_CONN_SEQ;
            uint32_t ack = get_seq(pkt) + 1;
            tju_packet_t *syn_ack_packet = create_packet(local_addr.port, remote_addr.port, seq, ack,
                                                         DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, SYN_FLAG_MASK, 1, 0, NULL, 0);
            char *msg = packet_to_buf(syn_ack_packet);
            sendToLayer3(msg, DEFAULT_HEADER_LEN);
            printf("发送syn_ack报文\n");
            return 0;
        }

        // 收到的是ACK报文 说明在握手的第三阶段 并意味着三次握手成功
        if (pkt_flag == ACK_FLAG_MASK)
        {
            if (pkt_ack == SERVER_CONN_SEQ + 1)
            {
                printf("收到客户端的ack报文\n");
                tju_tcp_t *temp_sock = deQueue(sock->incomplete_conn_queue);
                temp_sock->state = ESTABLISHED;
                enQueue(sock->complete_conn_queue, temp_sock);
                return 0;
            }
            else
            {
                printf("收到的syn_ack报文的ack不是服务端发送的SYN_ACK的seq+1\n");
            }
        }

        else
        {
            printf("该socket处于三次握手阶段 但是收到的报文不是三次握手报文\n");
            return 0;
        }
    }

    if (sock->state == SYN_SENT)
    {
        if (pkt_ack == CLIENT_CONN_SEQ + 1)
        {
            printf("收到syn_ack报文\n");
            uint32_t seq = pkt_ack;
            uint32_t ack = pkt_seq + 1;
            tju_sock_addr local_addr, target_addr;

            // 目标地址
            target_addr.ip = sock->established_remote_addr.ip;     //具体的IP地址
            target_addr.port = sock->established_remote_addr.port; //端口

            // 本地地址
            local_addr.ip = sock->established_local_addr.ip;     //具体的IP地址
            local_addr.port = sock->established_local_addr.port; //端口
            tju_packet_t *ack_packet = create_packet(local_addr.port, target_addr.port, seq, ack,
                                                     DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK, 1, 0, NULL, 0);
            char *msg2 = packet_to_buf(ack_packet);
            sendToLayer3(msg2, DEFAULT_HEADER_LEN);
            printf("发送ACK报文\n");
            sock->state = ESTABLISHED;
            return 0;
        }
        else
        {
            printf("收到的syn_ack报文的ACK不是seq+1 丢弃该报文\n");
            return 0;
        }
    }

    if (sock->state == ESTABLISHED)
    {
        // 表明收到了对方发来的数据报文
        if (pkt_flag == NO_FLAG)
        {
            while (pthread_mutex_lock(&(sock->recv_lock)) != 0)
                ; // 加锁

            // 收到的报文的序列号是期待的序列号
            if (pkt_seq == sock->window.wnd_recv->expect_seq)
            {
                uint32_t data_len = get_plen(pkt) - DEFAULT_HEADER_LEN;
                // 把收到的数据放到接受缓冲区

                if (TCP_BUF_SIZE - sock->received_len < data_len)
                {
                    printf("接收缓冲区已满 丢弃报文\n");
                    return 0;
                }

                if (sock->received_buf == NULL)
                {
                    sock->received_buf = malloc(data_len);
                }
                else
                {
                    sock->received_buf = realloc(sock->received_buf, sock->received_len + data_len);
                }
                memcpy(sock->received_buf + sock->received_len, pkt + DEFAULT_HEADER_LEN, data_len);
                sock->received_len += data_len;
                sock->window.wnd_recv->expect_seq = pkt_seq + data_len;

                uint32_t seq = sock->window.wnd_send->nextseq;
                uint32_t ack = sock->window.wnd_recv->expect_seq;
                uint16_t adv_window = TCP_BUF_SIZE - sock->received_len;

                tju_packet_t *pkt = create_packet(sock->established_local_addr.port, sock->established_remote_addr.port, seq, ack,
                                                  DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK, adv_window, 0, NULL, 0);

                char *msg = packet_to_buf(pkt);
                printf("收到seq = %d 的报文  发送ACK报文 ack = %d\n", pkt_seq, ack);
                sendToLayer3(msg, DEFAULT_HEADER_LEN + sizeof(struct timeval));

                pthread_mutex_unlock(&(sock->recv_lock)); // 解锁

                return 0;
            }
            else
            {

                uint32_t seq = sock->window.wnd_send->nextseq;
                uint32_t ack = sock->window.wnd_recv->expect_seq;
                uint16_t adv_window = TCP_BUF_SIZE - sock->received_len;
                char *msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, seq, ack,
                                              DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK, adv_window, 0, NULL, 0);
                sendToLayer3(msg, DEFAULT_HEADER_LEN);
                printf("收到seq = %d 丢弃报文 发送ACK报文 ack = %d\n", pkt_seq, ack);
                pthread_mutex_unlock(&(sock->recv_lock)); // 解锁

                return 0;
            }
        }

        // 收到的是ACK报文
        if (pkt_flag == ACK_FLAG_MASK)
        {
            while (pthread_mutex_lock(&(sock->send_lock)) != 0)
                ; // 接收缓冲区加锁

            // 收到的ack报文在发送窗口外 直接丢弃
            if (pkt_ack < sock->window.wnd_send->base)
            {
                printf("收到的ack报文在发送窗口外 丢弃报文 \n");
            }

            // 表示开始收到重复ACK
            else if (pkt_ack == sock->window.wnd_send->base)
            {

                printf("收到ACK报文 ack=%d\n", pkt_ack);

                sock->window.wnd_send->rwnd = get_advertised_window(pkt);

                if (sock->window.wnd_send->congestion_status == SLOW_START || sock->window.wnd_send->congestion_status == CONGESTION_AVOIDANCE)
                {
                    while (pthread_mutex_lock(&(sock->window.wnd_send->ack_cnt_lock)) != 0)
                        ;
                    sock->window.wnd_send->ack_cnt += 1;
                    pthread_mutex_unlock(&(sock->window.wnd_send->ack_cnt_lock));
                }

                if (sock->window.wnd_send->congestion_status == FAST_RECOVERY)
                {
                    sock->window.wnd_send->cwnd += MAX_DLEN;
                }

                if (sock->window.wnd_send->ack_cnt == 3 && sock->window.wnd_send->congestion_status != FAST_RECOVERY)
                {
                    sock->is_retransing = true;
                    sock->window.wnd_send->ssthresh = sock->window.wnd_send->rwnd / 2;
                    sock->window.wnd_send->cwnd = sock->window.wnd_send->ssthresh + 3 * MAX_DLEN;
                    sock->window.wnd_send->congestion_status = FAST_RECOVERY;
                    printf("收到3个重复ACK 开始快速重传\n");
                    RETRANS = 1;
                }
            }

            // 收到可用于更新的ACK
            else
            {
                printf("收到ACK报文 ack=%d\n", pkt_ack);

                while (pthread_mutex_lock(&(sock->window.wnd_send->ack_cnt_lock)) != 0);
                sock->window.wnd_send->ack_cnt = 0;
                pthread_mutex_unlock(&(sock->window.wnd_send->ack_cnt_lock));

                if (sock->window.wnd_send->congestion_status == SLOW_START)
                {
                    sock->window.wnd_send->cwnd += MAX_DLEN;
                    if (sock->window.wnd_send->cwnd >= sock->window.wnd_send->ssthresh)
                    {
                        sock->window.wnd_send->congestion_status = CONGESTION_AVOIDANCE;
                    }
                }
                else if (sock->window.wnd_send->congestion_status == CONGESTION_AVOIDANCE)
                {
                    sock->window.wnd_send->cwnd = sock->window.wnd_send->cwnd + MAX_DLEN * (MAX_DLEN / sock->window.wnd_send->cwnd);
                }
                else if (sock->window.wnd_send->congestion_status == FAST_RECOVERY)
                {
                    sock->window.wnd_send->cwnd = sock->window.wnd_send->ssthresh;
                    sock->window.wnd_send->congestion_status = CONGESTION_AVOIDANCE;
                }

                uint32_t free_len = pkt_ack - sock->window.wnd_send->base;

                if(sock->window.wnd_send->is_estimating_rtt)
                {
                    if (sock->window.wnd_send->rtt_expect_ack == pkt_ack)
                    {
                        TimeoutInterval(sock);
                    }
                    
                    sock->window.wnd_send->is_estimating_rtt = false;    
                }


                sock->window.wnd_send->base = pkt_ack;
                sock->window.wnd_send->rwnd = get_advertised_window(pkt);

                if (sock->window.wnd_send->base == sock->window.wnd_send->nextseq)
                {
                    stopTimer();
                }
                else
                {
                    stopTimer();
                    startTimer(sock);
                }

                char *new_buf = malloc(sock->sending_len - free_len);
                memcpy(new_buf, sock->sending_buf + free_len, sock->sending_len - free_len);
                free(sock->sending_buf);

                sock->sending_len -= free_len;
                sock->sending_buf_send_len -= free_len;
                sock->sending_buf = new_buf;

                // printf("发送窗口 base=%d, nextseq=%d\n", sock->window.wnd_send->base, sock->window.wnd_send->nextseq);
                // printf("发送缓冲区 sending_len=%d, sending_buf_send_len=%d\n", sock->sending_len, sock->sending_buf_send_len);
            }

            pthread_mutex_unlock(&(sock->send_lock)); // 解锁

            return 0;
        }

        // 收到 FIN 报文
        if (pkt_flag == FIN_ACK_FLAG_MASK)
        {
            printf("收到 FIN 报文\n");

            // 向客户端发送ACK
            uint32_t seq = sock->window.wnd_send->nextseq;
            uint32_t ack = pkt_seq + 1;
            char *msg1 = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, seq, ack,
                                           DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK, 1, 0, NULL, 0);
            sendToLayer3(msg1, DEFAULT_HEADER_LEN);

            sock->state = CLOSE_WAIT;

            printf("发送 ACK 报文\n");

            sleep(1);

            tju_close(sock);

            return 0;
        }
    }

    if (sock->state == FIN_WAIT_1)
    {
        if (pkt_flag == ACK_FLAG_MASK)
        {
            printf("收到 FIN_ACK 报文\n");
            sock->state = FIN_WAIT_2;
            return 0;
        }

        if (pkt_flag == FIN_ACK_FLAG_MASK)
        {

            printf("收到 FIN 报文\n");

            // 向客户端发送ACK
            uint32_t seq = sock->window.wnd_send->nextseq + 1;
            uint32_t ack = pkt_seq + 1;
            char *msg1 = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, seq, ack,
                                           DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK, 1, 0, NULL, 0);
            sendToLayer3(msg1, DEFAULT_HEADER_LEN);

            sock->state = CLOSING;

            printf("发送 ACK 报文\n");
        }
    }

    if (sock->state == FIN_WAIT_2)
    {
        if (pkt_flag == FIN_ACK_FLAG_MASK)
        {
            printf("收到 FIN 报文\n");
            uint32_t seq = pkt_ack;
            uint32_t ack = pkt_seq + 1;

            char *msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port,
                                          seq, ack, DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK, 1, 0, NULL, 0);
            sendToLayer3(msg, DEFAULT_HEADER_LEN);

            sleep(1);

            printf("发送 ACK 报文\n");
            sock->state = CLOSED;

            return 0;
        }
    }

    if (sock->state == LAST_ACK)
    {
        if (pkt_flag == FIN_ACK_FLAG_MASK)
        {
            printf("收到 ACK 报文\n");
            sock->state = CLOSED;

            return 0;
        }
    }

    if (sock->state == CLOSING)
    {
        if (pkt_flag == ACK_FLAG_MASK)
        {
            printf("收到 ACK 报文\n");
            sleep(1);
            sock->state = CLOSED;
            return 0;
        }
    }

    return 0;
}

int tju_close(tju_tcp_t *sock)
{
    uint32_t seq = sock->window.wnd_send->nextseq;
    char *msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, seq, 0,
                                  DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, FIN_ACK_FLAG_MASK, 1, 0, NULL, 0);
    sendToLayer3(msg, DEFAULT_HEADER_LEN);
    printf("发送FIN报文\n");

    if (sock->state == ESTABLISHED)
    {
        sock->state = FIN_WAIT_1;
    }
    else if (sock->state == CLOSE_WAIT)
    {
        sock->state = LAST_ACK;
    }

    // 等待四次挥手完成
    while (sock->state != CLOSED)
        ;

    sock = NULL;

    return 0;
}

/* 以下是对socket队列的操作 */
// 创建一个队列结点
sock_node *newNode(tju_tcp_t *sock)
{
    sock_node *temp = (sock_node *)malloc(sizeof(sock_node));
    temp->sock = sock;
    temp->next = NULL;
    return temp;
}

// 创建一个空队列
sock_queue *createQueue()
{
    sock_queue *q = (sock_queue *)malloc(sizeof(sock_queue));
    q->front = q->rear = NULL;
    q->queue_size = 0;
    return q;
}

// 入队操作
void enQueue(sock_queue *q, tju_tcp_t *sock)
{
    struct sock_node *temp = newNode(sock);

    if (q->rear == NULL)
    {
        q->front = q->rear = temp;
        q->queue_size++;
        // printf("    socket入队，size为：%d\n",q->queue_size);
        return;
    }

    q->rear->next = temp;
    q->rear = temp;
    q->queue_size++;
    // printf("    socket入队，size为：%d\n",q->queue_size);
    return;
}

// 出队操作 返回出队socket
tju_tcp_t *deQueue(sock_queue *q)
{
    if (q->front == NULL)
        return NULL;

    sock_node *temp = q->front;
    tju_tcp_t *sock = temp->sock;
    q->front = q->front->next;
    q->queue_size--;
    // printf("    socket出队，size为：%d\n",q->queue_size);

    // If front becomes NULL, then change rear also as NULL
    if (q->front == NULL)
        q->rear = NULL;

    free(temp);

    return sock;
}