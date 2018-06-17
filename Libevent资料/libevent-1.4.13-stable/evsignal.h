/*
 * Copyright 2000-2002 Niels Provos <provos@citi.umich.edu>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef _EVSIGNAL_H_
#define _EVSIGNAL_H_

typedef void (*ev_sighandler_t)(int);

//event_info的初始化包括，创建socket pair，设置ev_signal事件(但并没有注册，而是等到有信号注册时才检查并注册
//并将所有标记置零，初始化信号的注册事件链表指针等
struct evsignal_info {
	struct event ev_signal;    //为socket pair的读socket向event_base注册读事件时使用的event结构体
	int ev_signal_pair[2];      //socket pair对
	int ev_signal_added;       //记录ev_signal事件是否已经注册了
	volatile sig_atomic_t evsignal_caught;//是否有信号发生的标记，volatile类型，因为它会在另外的线程被修改
	struct event_list evsigevents[NSIG];   //数组，evsigevent[signo]表示注册到信号signo的事件链表
	sig_atomic_t evsigcaught[NSIG]; //具体记录每个信号触发的次数，evsigcaught[signo]表示signo信号被触发次数
#ifdef HAVE_SIGACTION
	struct sigaction **sh_old;//记录了原来signal处理函数的函数指针，当信号signo注册的event被清空时,
#else					   //需要重新设置其处理函数
	ev_sighandler_t **sh_old;
#endif
	int sh_old_max;    //旧的信号处理函数最大值标记
};
int evsignal_init(struct event_base *);
void evsignal_process(struct event_base *);
int evsignal_add(struct event *);
int evsignal_del(struct event *);
void evsignal_dealloc(struct event_base *);

#endif /* _EVSIGNAL_H_ */
