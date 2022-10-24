#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65536           // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000 // 监听的最大的事件数量

// 添加文件描述符
extern void addfd(int epollfd, int fd, bool one_shot);
extern void removefd(int epollfd, int fd);
//添加信号捕捉
/*
TCP是全双工信道, 可看作两条单工信道, TCP连接两端的两个端点各负责一条. 当对端调用close时, 虽然本意是关闭整个两条信道,
但本端只是收到FIN包. 按照TCP协议的语义, 表示对端只是关闭了其所负责的那一条单工信道, 仍然可以继续接收数据.
也就是说, 因为TCP协议的限制,一个端点无法获知对端的socket是调用了close还是shutdown.
对一个已经收到FIN包的socket调用read方法,如果接收缓冲已空, 则返回0, 这就是常说的表示连接关闭.
但第一次对其调用write方法时, 如果发送缓冲没问题, 会返回正确写入(发送).
但发送的报文会导致对端发送RST报文, 因为对端的socket已经调用了close, 完全关闭, 既不发送, 也不接收数据.
所以,第二次调用write方法(假设在收到RST之后), 会生成SIGPIPE信号, 导致进程退出.
为了避免进程退出, 可以捕获SIGPIPE信号, 或者忽略它, 给它设置SIG_IGN信号处理函数:addsig(SIGPIPE, SIG_IGN);
这样, 第二次调用write方法时, 会返回-1, 同时errno置为SIGPIPE. 程序便能知道对端已经关闭.
*/
// 说明handler是一个函数，右边(int)说明这个函数有一个int型参数，左边的void说明这个的函数值返回值是void型。
void addsig(int sig, void(handler)(int)) // (__sighandler_t)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;                 // 指定信号处理函数
    sigfillset(&sa.sa_mask);                 // 调用该函数后，set指向的信号集中将包含linux支持的64种信号，相当于64位都置1；
                                             // sa.sa_mask：设置进程的信号掩码，以指定哪些信号不能发送给本进程
    assert(sigaction(sig, &sa, NULL) != -1); // 为一个信号设置处理函数(更健壮的接口)；NULL：输出先前的信号处理方式
}

int main(int argc, char *argv[])
{
    int ret = 0;
    printf("webserver start\n");

    // 模拟proactor模式？？？
    //加判断

    // 终端传入 Port
    if (argc <= 1)
    { // 参数太少
        // argv[0] 调用程序时使用的程序名
        // basename 取文件名并打印文件名的最后一部分。 basename /etc/passwd => passwd
        printf("usage: %s port_number\n", basename(argv[0]));
        return 1;
    }

    //获取端口号
    int port = atoi(argv[1]);
    printf("[ LOG ] atoi\n");

    // 对SIGPIPE信号处理
    // 往一个读端关闭的管道或socket连接中写数据将引发SIGPIPE信号
    // SIG_IGN：忽略目标信号； #define SIG_IGN ((__sighandler_t) 1)
    addsig(SIGPIPE, SIG_IGN);
    printf("[ LOG ] addsig\n");

    //创建线程池，初始化线程池
    threadpool<http_conn> *pool = NULL;
    try
    {
        pool = new threadpool<http_conn>;
    }
    catch (...) // 处理try抛出的任何异常的代码；也可以通过`ExceptionName e` 指定
    {
        return 1;
    }
    printf("[ LOG ] threadpool\n");

    //创建数组用于保存所有的用户信息
    http_conn *users = new http_conn[MAX_FD];

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if (listenfd == -1)
    {
        printf("[ ERROR ] socket\n");
    }
    printf("[ LOG ] socket\n");

    // 端口复用允许在一个应用程序可以把n个套接字绑在一个端口上而不出错。 
    // 同时，这n 个套接字发送信息都正常，没有问题。 
    // 但是，这些套接字并不是所有都能读取信息，只有最后一个套接字会正常接收数据。 
    // 端口复用最常用的用途应该是防止服务器重启时之前绑定的端口还未释放或者程序突然退出而系统没有释放端口。
    int reuse = 1;
    // 设置socket的属性：fd, 通用socket选项, 复用地址, 1>复用 0>不可复用，前一个参数的长度
    ret = setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)); // ..., 1, sizeof(int)
    if (ret == -1)
    {
        printf("[ ERROR ] setsockopt\n");
    }
    printf("[ LOG ] setsockopt\n");

    //绑定
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY; // 所有IP
    address.sin_family = AF_INET;
    address.sin_port = htons(port); // port
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    if (ret == -1)
    {
        printf("[ ERROR ] bind\n");
    }
    printf("[ LOG ] bind\n");

    //监听
    ret = listen(listenfd, 5); //内核监听队列（全连接队列）的最大值，超长将不受理新连接；典型值 5
    if (ret == -1)
    {
        printf("[ ERROR ] listen\n");
    }
    printf("[ LOG ] listen\n");

    //==================================

    //创建epoll对象，事件数组， 添加
    /*
    struct epoll_event {
        uint32_t events; // Epoll 事件
        epoll_data_t data; // 用户数据，fd
    };    */
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5); // 随便填个数，已经没有意义了。 返回一个新fd（内核事件表），用于所有epoll的第一个参数
    if (epollfd == -1)
    {
        printf("[ ERROR ] epoll_create\n");
    }
    printf("[ LOG ] epoll_create\n");

    //将监听的文件描述符 添加到epoll对象中
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    while (true)
    {
        // events： 内核传出参数，保存了发生了事件的文件描述符的信息（检测后的结果）
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1); //-1 : *永久阻塞*
        // 如果程序在执行处于阻塞状态的系统调用时接收到信号，并且我们为该信号设置了信号处理函数，
        // 则默认情况下系统调用将被中断，并且errno被设置为EINTR。
        if ((num < 0) && (errno != EINTR))
        {
            printf("epoll failure\n");
            break;
        }
        // printf("[ LOG ] epoll_wait\n");

        //循环遍历事件数组；num由epoll_wait返回
        for (int i = 0; i < num; ++i)
        {
            int sockfd = events[i].data.fd;
            // 监听的fd
            if (sockfd == listenfd)
            {
                //有客户端连接进来
                struct sockaddr_in client_address;
                socklen_t client_len = sizeof(client_address);
                // client_address **传出**参数，记录了连接成功后客户端的地址信息（ip，port）
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_len);
                if (connfd < 0)
                {
                    printf("[ ERROR ] accept; errno is: %d\n", errno);
                    continue;
                }
                printf("[ LOG ] accept: connfd: %d\n", connfd);

                if (http_conn::m_user_count >= MAX_FD)
                {
                    //目前连接数满了
                    //给客户端写一个信息“服务器正忙”。
                    close(connfd);
                    continue;
                }
                // 将新的客户的数据初始化，放到数组中；
                users[connfd].init(connfd, client_address);
            }
            // EPOLLRDHUP:连接被对方关闭或对方关闭了写操作；挂起 事件；错误 事件
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                // 对方异常断开或错误事件
                users[sockfd].close_conn();
            }
            // 可读、可写 事件 【主要】
            else if (events[i].events & EPOLLIN)
            {
                //一次把所有数据都读出来
                if (users[sockfd].read())   // 1 > 2
                {
                    pool->append(users + sockfd);   // 2 > 3
                }
                else
                {
                    users[sockfd].close_conn();
                }
            }
            else if (events[i].events & EPOLLOUT)
            {
                if (!users[sockfd].write())
                {
                    users[sockfd].close_conn();
                }
            }
        }
    }

    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;
    return 0;
}