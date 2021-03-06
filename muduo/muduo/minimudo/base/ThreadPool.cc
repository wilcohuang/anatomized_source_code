// Use of this source code is governed by a BSD-style license
// that can be found in the License file.
//
// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/base/ThreadPool.h>

#include <muduo/base/Exception.h>

#include <boost/bind.hpp>
#include <assert.h>
#include <stdio.h>

using namespace muduo;

ThreadPool::ThreadPool(const string& nameArg)
  : mutex_(),
    notEmpty_(mutex_),    //初始化的时候需要把condition和mutex关联起来
    notFull_(mutex_),
    name_(nameArg),
    maxQueueSize_(0),   //初始化0
    running_(false)
{
}

ThreadPool::~ThreadPool()
{
  if (running_)    //如果线程池在运行，那就要进行内存处理，在stop()函数中执行
  {
    stop();
  }                  //如果没有分配过线程，那就不存在需要释放的内存，什么都不做就可以了
}

void ThreadPool::start(int numThreads)
{
  assert(threads_.empty());   //确保未启动过
  running_ = true;    //启动标志
  threads_.reserve(numThreads);   //预留reserver个空间
  for (int i = 0; i < numThreads; ++i)
  {
    char id[32];                  //id存储线程id
    snprintf(id, sizeof id, "%d", i+1);
    threads_.push_back(new muduo::Thread(   //boost::bind在绑定类内部成员时，第二个参数必须是类的实例
          boost::bind(&ThreadPool::runInThread, this), name_+id));//runInThread是每个线程的线程运行函数，线程为执行任务情况下会阻塞
    threads_[i].start();    //启动每个线程，但是由于线程运行函数是runInThread，所以会阻塞。
  }
  if (numThreads == 0 && threadInitCallback_) //如果线程池线程数为0，且设置了回调函数
  {
    threadInitCallback_();  //init回调函数
  }
}

void ThreadPool::stop()   //线程池停止
{
  {
  MutexLockGuard lock(mutex_);   //局部加锁
  running_ = false;
  notEmpty_.notifyAll(); //让阻塞在notEmpty contition上的所有线程执行完毕
  }
  for_each(threads_.begin(),
           threads_.end(),
           boost::bind(&muduo::Thread::join, _1));   //对每个线程调用，pthread_join(),防止资源泄漏
}

size_t ThreadPool::queueSize() const  //thread safe
{
  MutexLockGuard lock(mutex_);
  return queue_.size();
}

//运行一个任务//所以说线程池这个线程池执行任务是靠任务队列，客端需要执行一个任务，必须首先将该任务push进任务队列，等侯空闲线程处理
void ThreadPool::run(const Task& task)    
{
  if (threads_.empty())     //如果线程池为空，说明线程池为分配线程
  { 
    task();    //由当前线程执行
  }
  else
  {
    MutexLockGuard lock(mutex_);
    while (isFull())    //当任务队列满的时候，循环
    {
      notFull_.wait();    //一直等待任务队列不满   //这个锁在take()取任务函数中，取出任务队列未满，唤醒该锁
    }
    assert(!isFull()); 

    queue_.push_back(task);   //当任务队列不满，就把该任务加入线程池的任务队列
    notEmpty_.notify();  //唤醒take()取任务函数，让线程来取任务，取完任务后runInThread会执行任务
  }
}

#ifdef __GXX_EXPERIMENTAL_CXX0X__
void ThreadPool::run(Task&& task)
{
  if (threads_.empty())
  {
    task();
  }
  else
  {
    MutexLockGuard lock(mutex_);
    while (isFull())
    {
      notFull_.wait();   
    }
    assert(!isFull());

    queue_.push_back(std::move(task));
    notEmpty_.notify();
  }
}
#endif

//take函数是每个线程都执行的，需要考虑线程安全，考虑多线程下取任务的线程安全性，只能串行化
ThreadPool::Task ThreadPool::take()     //取任务函数
{
  MutexLockGuard lock(mutex_);   //注意，必须用锁
  // always use a while-loop, due to spurious wakeup     //防止惊群效应。
  while (queue_.empty() && running_)     //如果任务队列为空，并且线程池处于运行态
  {			//队列为空时没有使用线程池
    notEmpty_.wait();  //等待。条件变量需要用while循环，防止惊群效应。
  } 				   //因为所有的线程都在等同一condition，即notempty，只能有线程在wait返回时拿到mutex，并消耗资源
					  //其他线程虽然被notify同样返回，但资源已被消耗，queue为空(以1个任务为例)，其他线程就在while中继续等待
  Task task;
  if (!queue_.empty())    //从任务队列队头中取任务
  {
    task = queue_.front();
    queue_.pop_front();
    if (maxQueueSize_ > 0)    //？？如果未设置会等于0，不需要唤醒notFull
    {
      notFull_.notify();    //取出一个任务之后，如任务队列长度大于0，唤醒notfull未满锁
    }
  }
  return task;
}

bool ThreadPool::isFull() const   //not thread safe 
{
  mutex_.assertLocked(); //调用确保被使用线程锁住，因为isFull函数不是一个线程安全的函数,外部调用要加锁
  return maxQueueSize_ > 0 && queue_.size() >= maxQueueSize_;   //因为deque是通过push_back增加线程数目的，所以通过外界max_queuesize存储最多线程数目
}

//线程运行函数，无任务时都会阻塞在take()，有任务时会争互斥锁
void ThreadPool::runInThread()    //线程运行函数
{
  try
  {
    if (threadInitCallback_)
    {
      threadInitCallback_();    //支持每个线程运行前调度回调函数
    }
    while (running_)    //当线程池处于启动状态，一直循环
    {
      Task task(take());   //从任务队列中取任务，无任务会阻塞
      if (task)    //如果上面取出来了
      {
        task();    //做任务
      }
    }
  }
  catch (const Exception& ex)
  {
    fprintf(stderr, "exception caught in ThreadPool %s\n", name_.c_str());
    fprintf(stderr, "reason: %s\n", ex.what());
    fprintf(stderr, "stack trace: %s\n", ex.stackTrace());
    abort();
  }
  catch (const std::exception& ex)
  {
    fprintf(stderr, "exception caught in ThreadPool %s\n", name_.c_str());
    fprintf(stderr, "reason: %s\n", ex.what());
    abort();
  }
  catch (...)
  {
    fprintf(stderr, "unknown exception caught in ThreadPool %s\n", name_.c_str());
    throw; // rethrow
  }
}

