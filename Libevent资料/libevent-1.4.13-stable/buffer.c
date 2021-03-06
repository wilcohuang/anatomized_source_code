/*
 * Copyright (c) 2002, 2003 Niels Provos <provos@citi.umich.edu>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef WIN32
#include <winsock2.h>
#include <windows.h>
#endif

#ifdef HAVE_VASPRINTF
/* If we have vasprintf, we need to define this before we include stdio.h. */
#define _GNU_SOURCE
#endif

#include <sys/types.h>

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_STDARG_H
#include <stdarg.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "event.h"
#include "config.h"
#include "evutil.h"

struct evbuffer *
evbuffer_new(void)
{
	struct evbuffer *buffer;
	
	buffer = calloc(1, sizeof(struct evbuffer));  //动态分配一个evbuffer

	return (buffer);
}

void
evbuffer_free(struct evbuffer *buffer)
{
	if (buffer->orig_buffer != NULL)    //先判断orig_buffer是否需要释放，防止内存泄漏
		free(buffer->orig_buffer);
	free(buffer);
}

/* 
 * This is a destructive add.  The data from one buffer moves into
 * the other buffer.
 */

#define SWAP(x,y) do { \        //这个是传说中的单向swap???
	(x)->buffer = (y)->buffer; \
	(x)->orig_buffer = (y)->orig_buffer; \
	(x)->misalign = (y)->misalign; \
	(x)->totallen = (y)->totallen; \
	(x)->off = (y)->off; \
} while (0)

//移动数据从一个evbuffer到另一个evbuffer
int
evbuffer_add_buffer(struct evbuffer *outbuf, struct evbuffer *inbuf)
{
	int res;

	/* Short cut for better performance */
	if (outbuf->off == 0) {      //如果输出outbuf无有效数据，直接交换，无数据outbuf和有数据inbuf交换会清除inbuf中的数据
		struct evbuffer tmp;
		size_t oldoff = inbuf->off;

		/* Swap them directly */
		SWAP(&tmp, outbuf);    //这里就用了上面那个单向swap，用了3次
		SWAP(outbuf, inbuf);
		SWAP(inbuf, &tmp);

		/* 
		 * Optimization comes with a price; we need to notify the
		 * buffer if necessary of the changes. oldoff is the amount
		 * of data that we transfered from inbuf to outbuf
		 */
		if (inbuf->off != oldoff && inbuf->cb != NULL)     //如果inbuf->off!=oldoff说明交换成功，若设置回调就调用
			(*inbuf->cb)(inbuf, oldoff, inbuf->off, inbuf->cbarg);
		if (oldoff && outbuf->cb != NULL)   //如果老的oldoff有货，且输出outbuf设置就调用
			(*outbuf->cb)(outbuf, 0, oldoff, outbuf->cbarg);
		
		return (0);
	}

	res = evbuffer_add(outbuf, inbuf->buffer, inbuf->off);   //将in的evbuffer追加到outbuf中，这里不完美，如果inbuf->off为0，就不用调用
	if (res == 0) {
		/* We drain the input buffer on success */
		evbuffer_drain(inbuf, inbuf->off);
	}

	return (res);
}

int
evbuffer_add_vprintf(struct evbuffer *buf, const char *fmt, va_list ap)
{
	char *buffer;
	size_t space;
	size_t oldoff = buf->off;  
	int sz;
	va_list aq;

	/* make sure that at least some space is available */
	evbuffer_expand(buf, 64);    //64不是要直接去扩展64字节，而是用64作为基准去衡量有没有free空间
	for (;;) {                                  //如果连64字节都不够，才进行相应的扩展
		size_t used = buf->misalign + buf->off;   
		buffer = (char *)buf->buffer + buf->off;
		assert(buf->totallen >= used);
		space = buf->totallen - used;    //空闲空间

#ifndef va_copy
#define	va_copy(dst, src)	memcpy(&(dst), &(src), sizeof(va_list))   //va_list拷贝
#endif
		va_copy(aq, ap);

		sz = evutil_vsnprintf(buffer, space, fmt, aq);   //交给该函数实现

		va_end(aq);

		if (sz < 0)    //失败返回
			return (-1);
		if ((size_t)sz < space) {   //返回大小小于space
			buf->off += sz;       //更新偏移
			if (buf->cb != NULL)    //如果设置了，调用
				(*buf->cb)(buf, oldoff, buf->off, buf->cbarg);
			return (sz);
		}
		if (evbuffer_expand(buf, sz + 1) == -1)    //确保字符串和\0都被写入了buffer的有效地址，防止\0写入位置越界
			return (-1);

	}
	/* NOTREACHED */
}

//添加一个格式化的字符串到evbuffer尾部
int
evbuffer_add_printf(struct evbuffer *buf, const char *fmt, ...)  
{
	int res = -1;
	va_list ap;

	va_start(ap, fmt);
	res = evbuffer_add_vprintf(buf, fmt, ap);   //调用该函数实现
	va_end(ap);

	return (res);
}

/* Reads data from an event buffer and drains the bytes read */
//读取evbuffer缓冲区的数据到data中，长度为datlen
int
evbuffer_remove(struct evbuffer *buf, void *data, size_t datlen)
{
	size_t nread = datlen;
	if (nread >= buf->off)    //如果大，读已有的
		nread = buf->off;

	memcpy(data, buf->buffer, nread); 
	evbuffer_drain(buf, nread);    //同样调用消耗函数，清除已读数据
	
	return (nread);
}

/*
 * Reads a line terminated by either '\r\n', '\n\r' or '\r' or '\n'.
 * The returned buffer needs to be freed by the called.
 */
//读取以\r或\n结尾的一行数据
char *
evbuffer_readline(struct evbuffer *buffer)
{
	u_char *data = EVBUFFER_DATA(buffer); //(x)->buffer
	size_t len = EVBUFFER_LENGTH(buffer); //(x)->off,不知道为什么只有此处用了这两个宏，本文件其它可用的地方都没有用
	char *line;
	unsigned int i;

	for (i = 0; i < len; i++) {
		if (data[i] == '\r' || data[i] == '\n')
			break;
	}

	if (i == len)   //没找到\r或\n直接返回NULL
		return (NULL);

	if ((line = malloc(i + 1)) == NULL) {
		fprintf(stderr, "%s: out of memory\n", __func__);
		return (NULL);
	}

	memcpy(line, data, i);    //从buffer拷贝到line
	line[i] = '\0';

	/*
	 * Some protocols terminate a line with '\r\n', so check for
	 * that, too.
	 */
	if ( i < len - 1 ) {      //如果找到的小于len-1，有些协议可能存在\r\n情况，也要处理一下
		char fch = data[i], sch = data[i+1];

		/* Drain one more character if needed */
		if ( (sch == '\r' || sch == '\n') && sch != fch )
			i += 1;    //如果是\r\n，再往后走一个
	}

	evbuffer_drain(buffer, i + 1);    //注意参数i=1，因为drain函数中用buffer+=len得到新buffer的位置

	return (line);   //返回数据，注意这个line是用malloc申请的，用户需要手动释放!!!
}

/* Adds data to an event buffer */
//此函数是evbuffer的调整函数，将evbuffer的buffer段前移到起始位置orig_buffer
static void
evbuffer_align(struct evbuffer *buf)  
{
	memmove(buf->orig_buffer, buf->buffer, buf->off);    //直接调用memmove
	buf->buffer = buf->orig_buffer;
	buf->misalign = 0;            //更新misalign
}

/* Expands the available space in the event buffer to at least datlen */

int
evbuffer_expand(struct evbuffer *buf, size_t datlen)
{
	size_t need = buf->misalign + buf->off + datlen;   //先计算总需求

	/* If we can fit all the data, then we don't have to do anything */
	if (buf->totallen >= need)    //如果够，不扩展，直接返回
		return (0);

	/*
	 * If the misalignment fulfills our data needs, we just force an
	 * alignment to happen.  Afterwards, we have enough space.
	 */
	if (buf->misalign >= datlen) {  //如果前面0->misalign空间足够datlen，将evbuffer调整，将buffer段内数据前移
		evbuffer_align(buf);
	} else {                                     //不够就另外开辟一段空间
		void *newbuf;
		size_t length = buf->totallen;     //总大小

		if (length < 256)       //最少一次性开辟256字节
			length = 256;
		while (length < need)     //如果总大小都小于需求，就直接开辟2倍，类似于STL中vector的内存策略
			length <<= 1;

		if (buf->orig_buffer != buf->buffer)    //另外开辟前先把buffer前移到起始位置
			evbuffer_align(buf);
		if ((newbuf = realloc(buf->buffer, length)) == NULL)    //开辟另外的内存空间
			return (-1);

		buf->orig_buffer = buf->buffer = newbuf;   //将两个buffer指针更新到新的内存地址
		buf->totallen = length;    //更新总大小
	}

	return (0);
}

//将data追加到buffer中   
//这个函数有个bug，它没有判断datlen的大小，如果datlen=0，那么直接返回就行了
int
evbuffer_add(struct evbuffer *buf, const void *data, size_t datlen)
{
	size_t need = buf->misalign + buf->off + datlen;  //首先判断缓冲区大小，buf->off是已经存放的有效数据长度
	size_t oldoff = buf->off;

	if (buf->totallen < need) {    //如果总大小容纳不下datalen大小，扩充容量
		if (evbuffer_expand(buf, datlen) == -1)
			return (-1);
	}

	memcpy(buf->buffer + buf->off, data, datlen);    //将data追加到buffer+off后
	buf->off += datlen;    //off长度增加

	if (datlen && buf->cb != NULL)     //如果长度大于0且设置了回调函数，则调用回调函数
		(*buf->cb)(buf, oldoff, buf->off, buf->cbarg);  //没设置就什么也不做

	return (0);
}

//该函数有两个作用，其一是完全清除有效缓冲区,len设置为>=off即可，相当于有效缓冲区全部消耗掉
//其二是消耗一部分缓冲区，缓冲区前段消耗掉，相当于向后移动，misalign增大,off减小
void
evbuffer_drain(struct evbuffer *buf, size_t len)
{
	size_t oldoff = buf->off;

	if (len >= buf->off) {  //如果消耗的len的长度大于等于缓冲区off的长度，清空缓冲区
		buf->off = 0;
		buf->buffer = buf->orig_buffer;
		buf->misalign = 0;
		goto done;     //这个goto我暂时没看出来有什么用，我认为可以用if-else替换
	}

	//如果消耗的len不大于off，有效缓冲区向前移动，前面的一部分被读取，misalign增大，off减小�
	buf->buffer += len;
	buf->misalign += len;

	buf->off -= len;

 done:
	/* Tell someone about changes in this buffer */
	if (buf->off != oldoff && buf->cb != NULL)
		(*buf->cb)(buf, oldoff, buf->off, buf->cbarg);

}

/*
 * Reads data from a file descriptor into a buffer.
 */

#define EVBUFFER_MAX_READ	4096       //evbuffer的最大可读字节数

//值得注意evbuffer_read并不是相对evbuffer_write，它是从描述符往buffer读数据，evbuffer_remove才是读取evbuffer数据
//从fd上往buffer读取数据，如果缓冲区不够，则expand
int
evbuffer_read(struct evbuffer *buf, int fd, int howmuch)
{
	u_char *p;
	size_t oldoff = buf->off;
	int n = EVBUFFER_MAX_READ;    //最大字节数

#if defined(FIONREAD)  //FIONREAD返回缓冲区有多少个字节
#ifdef WIN32
	long lng = n;
	if (ioctlsocket(fd, FIONREAD, &lng) == -1 || (n=lng) <= 0) {
#else
	if (ioctl(fd, FIONREAD, &n) == -1 || n <= 0) {   //返回fd的可读字节数吧失败，或者n=0
#endif
		n = EVBUFFER_MAX_READ;    //如果获取缓冲区字节失败或n<=0,n取最大，因为要尽可能的最大读取
	} else if (n > EVBUFFER_MAX_READ && n > howmuch) {
		/*
		 * It's possible that a lot of data is available for
		 * reading.  We do not want to exhaust resources
		 * before the reader has a chance to do something
		 * about it.  If the reader does not tell us how much
		 * data we should read, we artifically limit it.  //人为限制
		 */
		if ((size_t)n > buf->totallen << 2)    //如果大于总大小的4倍，n赋值为它
			n = buf->totallen << 2;
		if (n < EVBUFFER_MAX_READ)
			n = EVBUFFER_MAX_READ;
	}
#endif	// 尽可能多的读取
	if (howmuch < 0 || howmuch > n)    //如果设置要读的字节数小于0或大于n，就让它读默认的4096个字节
		howmuch = n;     

	/* If we don't have FIONREAD, we might waste some space here */
	if (evbuffer_expand(buf, howmuch) == -1)  //如果不够4096就扩充
		return (-1);

	/* We can append new data at this point */
	p = buf->buffer + buf->off;    //p指向free space

#ifndef WIN32
	n = read(fd, p, howmuch);   //系统调用read
#else
	n = recv(fd, p, howmuch, 0);
#endif
	if (n == -1)      //失败-1返回
		return (-1);
	if (n == 0)              //如果为0返回0，读到0个字节
		return (0);

	buf->off += n;      //读到数据后，更新off

	/* Tell someone about changes in this buffer */
	if (buf->off != oldoff && buf->cb != NULL)   //如果读到数据且设置了回调就调用 
		(*buf->cb)(buf, oldoff, buf->off, buf->cbarg);

	return (n);   //返回读到的字节数
}

//把evbuffer的数据写入到文件描述符fd上，如果写入成功，调用evbuffer_drain删除已写数据
int
evbuffer_write(struct evbuffer *buffer, int fd)  
{
	int n;

#ifndef WIN32
	n = write(fd, buffer->buffer, buffer->off);  //直接写
#else
	n = send(fd, buffer->buffer, buffer->off, 0);
#endif
	if (n == -1)
		return (-1);
	if (n == 0)
		return (0);
	evbuffer_drain(buffer, n);    //写入到fd后，删除buffer中的数据，相当于消耗掉了

	return (n);
}

//查找字符串what
u_char *
evbuffer_find(struct evbuffer *buffer, const u_char *what, size_t len)
{
	u_char *search = buffer->buffer, *end = search + buffer->off;
	u_char *p;

	while (search < end &&                                        
	    (p = memchr(search, *what, end - search)) != NULL) {   //在search和end的范围内查找*what，注意这实在匹配第一个字符
		if (p + len > end)     //未找到
			break;
		if (memcmp(p, what, len) == 0)   //匹配到第一个字符后比较len长度的内存相等则返回，这两个函数组合也算是一种字符串匹配算法
			return (p);                              //话说这个查找算法可以用KMP
		search = p + 1;     //不相等后移一个字符，继续查找
	}

	return (NULL);
}

//缓冲区变化时设置的回调回掉函数
void evbuffer_setcb(struct evbuffer *buffer,
    void (*cb)(struct evbuffer *, size_t, size_t, void *),
    void *cbarg)
{
	buffer->cb = cb;
	buffer->cbarg = cbarg;
}
