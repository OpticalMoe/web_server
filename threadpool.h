#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "locker.h"

// 线程池类，将它定义为模板类是为了代码复用，模板参数T是任务类
template <typename T>
class threadpool
{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();

private:
    // 线程的数量
    int m_thread_number;

    // 描述线程池的数组，大小为m_thread_number
    pthread_t *m_threads;

    // 请求队列中最多允许的、等待处理的请求的数量
    int m_max_requests;

    // 请求队列
    std::list<T *> m_workqueue;

    // 保护请求队列的互斥锁
    locker m_queuelocker;

    // 是否有任务需要处理
    sem m_queuestat;

    // 是否结束线程
    bool m_stop;
};

template <typename T>
threadpool<T>::threadpool(int thread_number, int max_requests) : m_thread_number(thread_number), m_max_requests(max_requests),
                                                                 m_stop(false), m_threads(NULL)
{    
    printf("[ LOG ] threadpool<T>::threadpool\n");
    if ((thread_number <= 0) || (max_requests <= 0))
    {
        throw std::exception();
    }

    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
    {
        throw std::exception();
    }

    // 创建thread_number 个线程，并将他们设置为脱离线程。
    for (int i = 0; i < thread_number; ++i)
    {
        printf("create the %dth thread\n", i);
        /*
        - 参数
            tidp	：指向线程标识符的指针。
        　　*restrict_attr	：设置线程属性。
        　　(void*)	：线程运行函数的地址。
        　　arg		：运行函数的参数。
        - 返回值
        　　0 	：成功
            -1	：失败，并设置errno
        */
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }

        /*
        - 功能：主线程与子线程分离，子线程结束后，资源自动回收。
        - 参数：
            tid：线程标识符
        - 返回值：
        　　0 	：成功
            -1	：失败，并设置errno
        */
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
    m_stop = true;
}

template <typename T>
bool threadpool<T>::append(T *request)
{
    printf("[ LOG ] threadpool<T>::append\n");
    // 操作工作队列时一定要加锁，因为它被所有线程共享。
    m_queuelocker.lock();
    if (m_workqueue.size() > m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    // 可读、可写事件的 socket fd 加入队列 
    m_workqueue.push_back(request);     // 3 > 4: void threadpool<T>::run()
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

// 线程运行函数
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    printf("[ LOG ] *threadpool<T>::worker\n");
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}

template <typename T>
void threadpool<T>::run()
{
    printf("[ LOG ] threadpool<T>::run\n");
    // 定时器？自动运行的？？？
    while (!m_stop)
    {
        m_queuestat.wait();
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front();   // request:有读写事件的 socket fd
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
        {
            continue;
        }
        request->process();   // 4 > 5
    }
}

#endif
