#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include "lst_timer.h"

#define MAX_FD 65536   // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量
#define TIMESLOT 5

static int pipefd[2]; //管道两端的文件描述符
static sort_timer_lst timer_lst;
// 添加文件描述符
extern void addfd( int epollfd, int fd, bool one_shot );
extern void removefd( int epollfd, int fd );
extern int setnonblocking( int fd );

void sig_handler( int sig )
{
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

void timer_handler()
{
    // 定时处理任务，实际上就是调用tick()函数
    timer_lst.tick();
    // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
    alarm(TIMESLOT);
}

void addsig(int sig, void( handler )(int)){
    struct sigaction sa;
    //重置
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    sigfillset( &sa.sa_mask );
    //?????
    assert( sigaction( sig, &sa, NULL ) != -1 );
}
void addsig( int sig )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = sig_handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}
// 定时器回调函数，它删除非活动连接socket上的注册事件，并关闭之。
void cb_func( http_conn* user_data)
{
    printf( "close fd %d\n", user_data->getfd() );
    user_data->close_conn();
    
}

int main( int argc, char* argv[] ) {
    
    if( argc <= 1 ) {
        printf( "usage: %s port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi( argv[1] );
    //忽略sigpipe
    addsig( SIGPIPE, SIG_IGN );

    //线程池
    threadpool< http_conn >* pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    } catch( ... ) {
        return 1;
    }

    //多少个连接就有多少个文件描述符
    http_conn* users = new http_conn[ MAX_FD ];

    //创建通信套接字
    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );

    int ret = 0;
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons( port );

    // 端口复用，必须在绑定之前设置
    int reuse = 1;
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    //绑定
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    //监听
    ret = listen( listenfd, 5 );

    // 创建epoll对象，和事件数组，添加
    epoll_event events[ MAX_EVENT_NUMBER ];
    int epollfd = epoll_create( 5 );
    // 将监听描述符添加到epoll对象中
    addfd( epollfd, listenfd, false );
    // ???
    http_conn::m_epollfd = epollfd;

    // 创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert( ret != -1 );
    setnonblocking( pipefd[1] );
    addfd( epollfd, pipefd[0] , false);

    // 设置信号处理函数
    addsig(SIGALRM);
    addsig(SIGTERM);
    bool stop_server = false;
    bool timeout = false;
    alarm(TIMESLOT);  // 定时,5秒后产生SIGALARM信号

    while(true) {
        
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        //如果没有检测成功且不是EINTR(阻塞)，则说明调用epoll失败
        if ( ( number < 0 ) && ( errno != EINTR ) ) {
            printf( "epoll failure\n" );
            break;
        }

        //遍历已经活跃的文件描述符
        for ( int i = 0; i < number; i++ ) {
            
            int sockfd = events[i].data.fd;
            //如果是监听描述符，则说明有新的客户端请求接入
            if( sockfd == listenfd ) {
                //创建客户端socket地址数据结构
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                //接受监听描述符得到的客户端信息，获取客户端文件描述符
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                
                //如果没接受到，输出错误号
                if ( connfd < 0 ) {
                    printf( "errno is: %d\n", errno );
                    continue;
                } 

                //用户数量超过了最大文件描述符数量，则关闭连接
                if( http_conn::m_user_count >= MAX_FD ) {
                    close(connfd);
                    continue;
                }
                // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中
                util_timer* timer = new util_timer;
                timer->user_data = &users[connfd];
                timer->cb_func = cb_func;
                time_t cur = time( NULL );
                timer->expire = cur + 3 * TIMESLOT;
                timer_lst.add_timer( timer );
                //用客户端描述符和sock信息初始化http_conn连接
                users[connfd].init( connfd, client_address,timer);

            } 
            else if( ( sockfd == pipefd[0] ) && ( events[i].events & EPOLLIN ) ) {
                // 处理信号
                int sig;
                char signals[1024];
                ret = recv( pipefd[0], signals, sizeof( signals ), 0 );
                if( ret == -1 ) {
                    continue;
                } else if( ret == 0 ) {
                    continue;
                } else  {
                    for( int i = 0; i < ret; ++i ) {
                        switch( signals[i] )  {
                            case SIGALRM:
                            {
                                // 用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                                // 这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
            }
            //通过EPOLLRDHUP(读关闭)属性，来判断是否对端已经关闭(对端发送 FIN)，这样可以减少一次系统调用
            //EPOLLHUP(读写关闭)，本(对)端调用shutdown()或者本地服务器崩溃等
            //EPOLLERR 服务器读写异常（可能是对方异常断开导致）
            else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) {
                if(users[sockfd].gettimer()){
                    timer_lst.del_timer(users[sockfd].gettimer());
                }
                users[sockfd].close_conn();
            } 
            //输入事件
            else if(events[i].events & EPOLLIN) {
                if(users[sockfd].read()) {
                    // 如果某个客户端上有数据可读，则我们要调整该连接对应的定时器，以延迟该连接被关闭的时间。
                    if( util_timer* timer = users[sockfd].gettimer() ) {
                        time_t cur = time( NULL );
                        timer->expire = cur + 3 * TIMESLOT;
                        printf( "adjust timer once\n" );
                        timer_lst.adjust_timer(timer);
                    }
                    //向线程池队列中添加读请求
                    pool->append(users + sockfd);
                } else {
                    if(users[sockfd].gettimer()){
                        timer_lst.del_timer(users[sockfd].gettimer());
                    }
                    users[sockfd].close_conn();
                }

            }  
            //输出事件
            else if( events[i].events & EPOLLOUT ) {
                //写操作
                if( !users[sockfd].write() ) {
                    if(users[sockfd].gettimer()){
                        timer_lst.del_timer(users[sockfd].gettimer());
                    }
                    users[sockfd].close_conn();
                } 
            }

        }
        // 最后处理定时事件，因为I/O事件有更高的优先级。当然，这样做将导致定时任务不能精准的按照预定的时间执行。
        if( timeout ) {
            timer_handler();
            timeout = false;
        }
    }
    
    close( epollfd );
    close( listenfd );
    close( pipefd[1] );
    close( pipefd[0] );
    delete [] users;
    delete pool;
    return 0;
}