// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/Acceptor.h>

#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/SocketsOps.h>

#include <boost/bind.hpp>

#include <errno.h>
#include <fcntl.h>
//#include <sys/types.h>
//#include <sys/stat.h>

using namespace muduo;
using namespace muduo::net;

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reuseport)
  : loop_(loop),
    acceptSocket_(sockets::createNonblockingOrDie(listenAddr.family())), //创建监听套接字
    acceptChannel_(loop, acceptSocket_.fd()),  //绑定Channel和socketfd
    listenning_(false),  
    idleFd_(::open("/dev/null", O_RDONLY | O_CLOEXEC))  //预先准备一个空闲文件描述符
{
  assert(idleFd_ >= 0);  //>=0
  acceptSocket_.setReuseAddr(true);
  acceptSocket_.setReusePort(reuseport);
  acceptSocket_.bindAddress(listenAddr);
  acceptChannel_.setReadCallback(                        
      boost::bind(&Acceptor::handleRead, this));   //设置Channel的fd的读回调函数
}

Acceptor::~Acceptor()
{
  acceptChannel_.disableAll();   //需要把所有事件都disable掉，才能调用remove函数
  acceptChannel_.remove();
  ::close(idleFd_);    //关闭文件描述符
}

//Acceptor是在listen中开始关注可读事件
void Acceptor::listen()
{
  loop_->assertInLoopThread();
  listenning_ = true;
  acceptSocket_.listen();
  acceptChannel_.enableReading();   //关注可读事件
}

void Acceptor::handleRead()    //读回调函数
{
  loop_->assertInLoopThread();
  InetAddress peerAddr;
  //FIXME loop until no more
  int connfd = acceptSocket_.accept(&peerAddr);   //获得已连接套接字
  if (connfd >= 0)
  {
    // string hostport = peerAddr.toIpPort();
    // LOG_TRACE << "Accepts of " << hostport;
    if (newConnectionCallback_)   //如果设置了新临街回调函数
    {
      newConnectionCallback_(connfd, peerAddr);   //那么就执行它
    }
    else
    {
      sockets::close(connfd);  //否则就关闭
    }
  }
  else   //如果<0失败了�
  {
    LOG_SYSERR << "in Acceptor::handleRead";
    // Read the section named "The special problem of
    // accept()ing when you can't" in libev's doc.
    // By Marc Lehmann, author of livev.
    if (errno == EMFILE)   //太多的文件描述符
    {
      ::close(idleFd_);   //先关闭空闲文件描述符，让它能够接收。否则由于采用电平触发，不接收会一直触发。
      idleFd_ = ::accept(acceptSocket_.fd(), NULL, NULL);  //那就腾出一个文件描述符，用来accept
      ::close(idleFd_);  //accept之后再关闭
      idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);  //然后再打开成默认方式
    }
  }
}

