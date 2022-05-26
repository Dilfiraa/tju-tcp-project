#ifndef _GLOBAL_H_
#define _GLOBAL_H_

#include <netinet/in.h>
#include <math.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "global.h"
#include <pthread.h>
#include <sys/select.h>
#include <arpa/inet.h>


#define min(a,b) (((a) < (b)) ? (a) : (b))

// 单位是byte
#define SIZE32 4
#define SIZE16 2
#define SIZE8  1

// 一些Flag
#define NO_FLAG 0
#define NO_WAIT 1
#define TIMEOUT 2
#define TRUE 1
#define FALSE 0

// 定义最大包长 防止IP层分片
#define MAX_DLEN 1375 	// 最大包内数据长度
#define MAX_LEN 1400 	// 最大包长度

// TCP socket 状态定义
#define CLOSED 0
#define LISTEN 1
#define SYN_SENT 2
#define SYN_RECV 3
#define ESTABLISHED 4
#define FIN_WAIT_1 5
#define FIN_WAIT_2 6
#define CLOSE_WAIT 7
#define CLOSING 8
#define LAST_ACK 9
#define TIME_WAIT 10

// TCP 拥塞控制状态
#define SLOW_START 0
#define CONGESTION_AVOIDANCE 1
#define FAST_RECOVERY 2

// TCP 缓冲区大小
#define TCP_BUF_SIZE 44*MAX_DLEN 

// TCP 发送窗口大小
#define TCP_SENDWN_SIZE 16*MAX_DLEN // 比如最多放32个满载数据包

// 三次握手过程中报文的序列号

#define CLIENT_CONN_SEQ 0
#define SERVER_CONN_SEQ 0


uint32_t RETRANS;
uint32_t TIMEOUT_FLAG;

// TCP 发送窗口
// 注释的内容如果想用就可以用 不想用就删掉 仅仅提供思路和灵感
typedef struct {
	uint16_t window_size;
	uint32_t base;
    uint32_t nextseq;
    uint64_t estmated_rtt;
	uint64_t dev_rtt;
  	int ack_cnt;
	uint32_t last_ack;  
    pthread_mutex_t ack_cnt_lock;
    struct itimerval timeout;
	uint16_t rwnd; 
   	int congestion_status;
  	uint16_t cwnd; 
    uint16_t ssthresh;
	bool is_estimating_rtt;		// 来表明是否在测量SampleRTT
	struct timeval send_time;	// 记录发送时间
	uint32_t rtt_expect_ack;	// 用来测量RTT的报文期待的ACK号

} sender_window_t;

// TCP 接受窗口
// 注释的内容如果想用就可以用 不想用就删掉 仅仅提供思路和灵感
typedef struct {
//	char received[TCP_RECVWN_SIZE];
//   received_packet_t* head;
//   char buf[TCP_RECVWN_SIZE];
//   uint8_t marked[TCP_RECVWN_SIZE];
   uint32_t expect_seq;
} receiver_window_t;

// TCP 窗口 每个建立了连接的TCP都包括发送和接受两个窗口
typedef struct {
	sender_window_t* wnd_send;
  	receiver_window_t* wnd_recv;
} window_t;

typedef struct {
	uint32_t ip;
	uint16_t port;
} tju_sock_addr;

// 结构和数据结构体声明
struct sock_node;
struct sock_queue;
typedef struct sock_node sock_node;
typedef struct sock_queue sock_queue;

// TJU_TCP 结构体 保存TJU_TCP用到的各种数据
typedef struct {
	int state; // TCP的状态

	tju_sock_addr bind_addr; // 存放bind和listen时该socket绑定的IP和端口
	tju_sock_addr established_local_addr; // 存放建立连接后 本机的 IP和端口
	tju_sock_addr established_remote_addr; // 存放建立连接后 连接对方的 IP和端口

	pthread_mutex_t send_lock; // 发送数据锁
	char* sending_buf; // 发送数据缓存区起始地址
	int sending_buf_send_len;	// 已发送数据的长度
	int sending_len; // 发送数据缓存长度

	pthread_mutex_t recv_lock; // 接收数据锁
	char* received_buf; // 接收数据缓存区
	int received_len; // 接收数据缓存长度

	pthread_cond_t wait_cond; // 可以被用来唤醒recv函数调用时等待的线程

	window_t window; // 发送和接受窗口

	bool is_retransing;		// 表明是否在重传

	sock_queue* incomplete_conn_queue;		//半连接队列
	sock_queue* complete_conn_queue;		//全连接队列

} tju_tcp_t;

// 队列结点的结构体定义
typedef struct sock_node{
	tju_tcp_t* sock;		//数据域 存放的是socket
	struct sock_node* next; //指向队列的下一个节点
}sock_node;

// 队列的结构体定义(未完成队列和已完成队列)
typedef struct sock_queue{
	struct sock_node *front, *rear;
	int queue_size;
}sock_queue;


#endif