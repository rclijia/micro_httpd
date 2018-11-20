#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>

/*
	A simple HTTP server implemented in C language,  
	used to help beginners understand socket, multithreading and other related knowledge.
*/
#define DEBUG 1
#if DEBUG
#define DPRINT(fmt, args...) fprintf(stdout, fmt, ##args)
#else
#define DPRINT(fmt, args...)
#endif

#define THREAD_NUM			4  /* can be adjusted according to the hardware  */
#define SERVER_LISTEN_PORT  80
#define MAX_BUF_LEN			(2048)
#define HTTPD_SERVER_IP		"127.0.0.1"

typedef unsigned int			MHD_ATOMIC_T;
#define MHD_ATOMIC_SET(v, i)	__sync_lock_test_and_set((&v), i)
#define MHD_ATOMIC_READ(v)		__sync_or_and_fetch((&v), 0)
#define MHD_ATOMIC_INC(v) 		__sync_add_and_fetch((&v),1)
#define MHD_ATOMIC_DEC(v) 		__sync_sub_and_fetch((&v),1)
#define MHD_ATOMIC_ADD(v,add) 	__sync_add_and_fetch((&v),(add))
#define MHD_ATOMIC_SUB(v,sub) 	__sync_sub_and_fetch((&v),(sub))

#define HTTP_MAINPAGE_RES_HDR "HTTP/1.1 200 OK\r\nServer: MicroHttpServer-0.2.3\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n"
#define HTTP_MAINPAGE_CONT "<html><head><title>Hello, Micro Http!</title></head><body><h1>Hello, Micro Http!</h1></body></html>"

static int httpd = -1;

static pthread_t g_thread_pool[THREAD_NUM];
static MHD_ATOMIC_T thread_in_use[THREAD_NUM];
static volatile int thread_client_fd[THREAD_NUM];

enum thread_state{
	TSTATE_IDLE = 0,
	TSTATE_PREPARE,
	TSTATE_TO_WORK,
};

static int mhd_start(void)
{
    int on = 1;
    struct sockaddr_in name;

    signal(SIGPIPE, SIG_IGN);

    httpd = socket(PF_INET, SOCK_STREAM, 0);
    if (httpd == -1){
        perror("socket");
        return -1;
    }
    memset(&name, 0, sizeof(name));
    name.sin_family = AF_INET;
    name.sin_port = htons(SERVER_LISTEN_PORT);
    name.sin_addr.s_addr = htonl(INADDR_ANY);
    
    if ((setsockopt(httpd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on))) < 0){  
        perror("setsockopt failed");
        return -1;
    }
    if (bind(httpd, (struct sockaddr *)&name, sizeof(name)) < 0){
        perror("bind");
        return -1;
    }

    if (listen(httpd, THREAD_NUM) < 0){
        perror("listen");
        return -1;
     }
        
    return 0;
}

static inline int mhd_request_check(const char *data, int data_len)
{
	if(data_len < strlen("GET / HTTP/1.1\r\n\r\n")){ /* Less than minimum HTTP legal request length */
		return -1;
	}

	if(memmem(data, data_len, "\r\n\r\n", 4) == NULL){ /* no http end flag */
		return -1;
	}

	return 0;
}

static int mdh_process_request(int client_fd, char *rcv_buf, int buf_len)
{
	int rcv_len;
	
	rcv_len = read(client_fd, rcv_buf, buf_len);
	if(rcv_len < 0){
		DPRINT("client_fd:%d recv error, ret = %d, exit!\n", client_fd, rcv_len);
		return -1;	
	}

	if(mhd_request_check(rcv_buf, rcv_len) < 0){
		return -1;
	}

	return rcv_len;
}

static int mdh_send_response(int client_fd)
{
	char http_res_header[MAX_BUF_LEN];
	int ret;

	snprintf(http_res_header, MAX_BUF_LEN, HTTP_MAINPAGE_RES_HDR, "text/html", (int)strlen(HTTP_MAINPAGE_CONT));

	/* send http response header */
	ret = write(client_fd, http_res_header, strlen(http_res_header));
	if(ret != strlen(http_res_header)){
		DPRINT("sock write error, ret=%d!\n", ret);
		return -1;
	}

	/* send http response content */
	ret = write(client_fd, HTTP_MAINPAGE_CONT, strlen(HTTP_MAINPAGE_CONT));
	if(ret != strlen(HTTP_MAINPAGE_CONT)){
		DPRINT("sock write error, ret=%d!\n", ret);
		return -1;
	}

	return 0;
}

static void mhd_do_work(int client_fd)
{
	char rcv_buf[MAX_BUF_LEN];
	int rcv_len;
	
	rcv_len = mdh_process_request(client_fd, rcv_buf, MAX_BUF_LEN);
	if(rcv_len < 0){
		goto done;
	}

	mdh_send_response(client_fd);

done:
	close(client_fd);
	return;
}

static void *mhd_work_thread(void *arg)
{
	int tid = *((int *)arg);
	
	while(1){
		if(TSTATE_TO_WORK != MHD_ATOMIC_READ(thread_in_use[tid])){
			usleep(1000);
			continue;
		}

		DPRINT("thread %d ready to work, client fd is:%d\n", tid, thread_client_fd[tid]);

		mhd_do_work(thread_client_fd[tid]);

		thread_client_fd[tid] = -1;

		DPRINT("thread %d word is done, wait for new work......\n", tid);

		MHD_ATOMIC_SET(thread_in_use[tid], TSTATE_IDLE);
	}
	return NULL;

}

static int mhd_thread_pool_init(void)
{
	int i;
	static int __tid[THREAD_NUM];
	for(i = 0; i < THREAD_NUM; i++){
		__tid[i] = i;
		pthread_create(&g_thread_pool[i], NULL, mhd_work_thread, &__tid[i]);
	}

	return 0;
}

static void mhd_search_idle_thread(int client_fd)
{
	int i;
	int done = 0;

	while(0 == done){
		for(i = 0; i < THREAD_NUM; i++){
			if(TSTATE_IDLE == MHD_ATOMIC_READ(thread_in_use[i])){
				MHD_ATOMIC_SET(thread_in_use[i], TSTATE_PREPARE); 
				thread_client_fd[i] = client_fd;
				MHD_ATOMIC_SET(thread_in_use[i], TSTATE_TO_WORK);
				done = 1;
				DPRINT("thread %d idle, assign some work to it.....\n", i);
				return;
			}
		}
		usleep(1000); /* no idle thread, wait a moment..... */
	}

	return;
}

int main(void)
{
	int ret;
	int client_fd = -1;
	struct sockaddr_in client_addr;
	socklen_t client_addr_len;

	srand(time(NULL));

	ret = mhd_start();
	if(ret < 0){
		exit(1);
	}

	mhd_thread_pool_init();
	
	DPRINT("mhttpd starting....\n");

	while(1){
		client_fd = accept(httpd, (struct sockaddr *)&client_addr, &client_addr_len);
		if(client_fd > 0){
			mhd_search_idle_thread(client_fd);
		}
	}
	
	return 0;
}

