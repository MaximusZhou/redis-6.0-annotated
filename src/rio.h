/*
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2009-2019, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#ifndef __REDIS_RIO_H
#define __REDIS_RIO_H

#include <stdio.h>
#include <stdint.h>
#include "sds.h"
#include "connection.h"

#define RIO_FLAG_READ_ERROR (1<<0)
#define RIO_FLAG_WRITE_ERROR (1<<1)

struct _rio {
    /* Backend functions.
     * Since this functions do not tolerate short writes or reads the return
     * value is simplified to: zero on error, non zero on complete success. */
	/*
	 * 这些读写函数不允许short 读或写，因此则返回值为0表示发生错误，为1表示成功完成
	 */
    size_t (*read)(struct _rio *, void *buf, size_t len);
    size_t (*write)(struct _rio *, const void *buf, size_t len);
    off_t (*tell)(struct _rio *);
    int (*flush)(struct _rio *);
    /* The update_cksum method if not NULL is used to compute the checksum of
     * all the data that was read or written so far. The method should be
     * designed so that can be called with the current checksum, and the buf
     * and len fields pointing to the new block of data to add to the checksum
     * computation. */
	/* 用于计算所有读取数据的checksum的函数，比如在RDB文件存盘的时候，
	 * 这个字段被设置为rioGenericUpdateChecksum，
	 * 他要求这个计算checksum的函数，可以一边读或写数据，一边计算checksum，
	 * 而不是等所有的数据读写完成后，才计算checksum */
    void (*update_cksum)(struct _rio *, const void *buf, size_t len);

    /* The current checksum and flags (see RIO_FLAG_*) */
    uint64_t cksum, flags;

    /* number of bytes read or written */
	/* 当前读或者写的总字节数 */
    size_t processed_bytes;

    /* maximum single read or write chunk size */
	/* 每次读写调用最多操作的字节数，默认值为0，表示没限制 */
    size_t max_processing_chunk;

    /* Backend-specific vars. */
	/* 写入或者读取各种不同类型的存盘介质，用不同的结构体 */
	/* 下面使用的off_t类型不是C标准，是POSIX标准定义的，是一个有符号的整数，
	 * 用来表示文件大小，并且其大小在不同的机器上不同的，参考：
	 * https://stackoverflow.com/questions/9073667/where-to-find-the-complete-definition-of-off-t-type*/
    union {
        /* In-memory buffer target. */
		/* 目标是内存buff */
        struct {
            sds ptr;
            off_t pos; 
        } buffer;
		/* 目标是标准IO */
        /* Stdio file pointer target. */
        struct {
            FILE *fp;
			/* buffered，自从最近一次调用fsync以来，通过fwrite写入的字节数 */
            off_t buffered; /* Bytes written since last fsync. */
			/* 是否自动调用fsync，比如在RDB文件的时候，通过配置 rdb-save-incremental-fsync决定的，
			 * 默认值为32M，即buffered的值大于等于这个值，则调用fsync，把应用层和内核中的数据刷到磁盘上 */
            off_t autosync; /* fsync after 'autosync' bytes written. */
        } file;
        /* Connection object (used to read from socket) */
		/* 目标是套接字 */
        struct {
            connection *conn;   /* Connection */
            off_t pos;    /* pos in buf that was returned */
            sds buf;      /* buffered data */
            size_t read_limit;  /* don't allow to buffer/read more than that */
            size_t read_so_far; /* amount of data read from the rio (not buffered) */
        } conn;
        /* FD target (used to write to pipe). */
		/* 目标是pipe，用于写 */
        struct {
            int fd;       /* File descriptor. */
            off_t pos;
            sds buf;
        } fd;
    } io;
};

typedef struct _rio rio;

/* The following functions are our interface with the stream. They'll call the
 * actual implementation of read / write / tell, and will update the checksum
 * if needed. */
/*
 * rioWrite 和 rioRead 就是供外部调用的接口，用来向相应的设备上读写数据，
 * 实质就是调用函数指针的封装，同时会计算读写数据的checksum
 */
static inline size_t rioWrite(rio *r, const void *buf, size_t len) {
    if (r->flags & RIO_FLAG_WRITE_ERROR) return 0;
    while (len) {
        size_t bytes_to_write = (r->max_processing_chunk && r->max_processing_chunk < len) ? r->max_processing_chunk : len;
        if (r->update_cksum) r->update_cksum(r,buf,bytes_to_write);
        if (r->write(r,buf,bytes_to_write) == 0) {
            r->flags |= RIO_FLAG_WRITE_ERROR;
            return 0;
        }
        buf = (char*)buf + bytes_to_write;
        len -= bytes_to_write;
        r->processed_bytes += bytes_to_write;
    }
    return 1;
}

static inline size_t rioRead(rio *r, void *buf, size_t len) {
    if (r->flags & RIO_FLAG_READ_ERROR) return 0;
    while (len) {
        size_t bytes_to_read = (r->max_processing_chunk && r->max_processing_chunk < len) ? r->max_processing_chunk : len;
        if (r->read(r,buf,bytes_to_read) == 0) {
            r->flags |= RIO_FLAG_READ_ERROR;
            return 0;
        }
        if (r->update_cksum) r->update_cksum(r,buf,bytes_to_read);
        buf = (char*)buf + bytes_to_read;
        len -= bytes_to_read;
        r->processed_bytes += bytes_to_read;
    }
    return 1;
}

static inline off_t rioTell(rio *r) {
    return r->tell(r);
}

static inline int rioFlush(rio *r) {
    return r->flush(r);
}

/* This function allows to know if there was a read error in any past
 * operation, since the rio stream was created or since the last call
 * to rioClearError(). */
static inline int rioGetReadError(rio *r) {
    return (r->flags & RIO_FLAG_READ_ERROR) != 0;
}

/* Like rioGetReadError() but for write errors. */
static inline int rioGetWriteError(rio *r) {
    return (r->flags & RIO_FLAG_WRITE_ERROR) != 0;
}

static inline void rioClearErrors(rio *r) {
    r->flags &= ~(RIO_FLAG_READ_ERROR|RIO_FLAG_WRITE_ERROR);
}

/* 供外部模块调用，用来初始化变量rio，即准备读写设备的函数，已经数据成员初始化 */
void rioInitWithFile(rio *r, FILE *fp);
void rioInitWithBuffer(rio *r, sds s);
void rioInitWithConn(rio *r, connection *conn, size_t read_limit);
void rioInitWithFd(rio *r, int fd);

void rioFreeFd(rio *r);
void rioFreeConn(rio *r, sds* out_remainingBufferedData);

/* 封装rioWrite接口，方便应用层写不同类型的数据 */
size_t rioWriteBulkCount(rio *r, char prefix, long count);
size_t rioWriteBulkString(rio *r, const char *buf, size_t len);
size_t rioWriteBulkLongLong(rio *r, long long l);
size_t rioWriteBulkDouble(rio *r, double d);

struct redisObject;
int rioWriteBulkObject(rio *r, struct redisObject *obj);

/* 通用的计算checksum的函数，支持一边读写，一边计算checksum */
void rioGenericUpdateChecksum(rio *r, const void *buf, size_t len);
/* 设置写数据的时候，自动调用fsync，防止OS缓存太大，一次刷到磁盘上，出现卡顿的情况 */
void rioSetAutoSync(rio *r, off_t bytes);

#endif
