/*
 *@file    stm_lfq.h

 *进程模型事件服务接口头文件

 *@since   
 *@author 
*/

#ifndef _HAP_STM_LFQ_H
#define _HAP_STM_LFQ_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <stdint.h>            

#ifdef __cplusplus
extern "C" {
#endif

// 数据类型定义 /
typedef int16_t         int16;
typedef uint16_t        uint16;
typedef int32_t         int32;
typedef uint32_t        uint32;
typedef long long         int64;
typedef unsigned long long       uint64;
typedef int16           BOOL;
typedef int32_t   ResCodeT;

// 模块STM中的错误 /
#define ERCD_HAP_STM_LFQ_INIT_FAILED                    0x10080002      //< LFQ初始化失败/
#define ERCD_HAP_STM_LFQ_WRITE_FULL                     0x10080003      //< LFQ队列写满/
#define ERCD_HAP_STM_LFQ_INVALID_POS                    0x10080004      //< LFQ队列读写位置无效/
#define ERCD_HAP_STM_LFQ_WRITE_TIMEOUT                  0x10080005      //< LFQ队列写超时失败/
#define ERCD_HAP_STM_LFQ_METHOD_NOT_SUPPORT             0x10080006      //< LFQ队列接口不支持该类型队列/
#define ERCD_HAP_STM_LFQ_QUEUE_LEN_ERR                  0x10080007      //< LFQ队列接口不支持该类型队列/
#define ERCD_HAP_STM_LFQ_ARG                            0x10080020      //< LFQ队列传参错误/
#define ERCD_HAP_STM_LFQ_TRY_AGAIN                      0x10080021      //< LFQ队列为空/
#define ERCD_HAP_STM_LFQ_BAD_SLOT                       0x10080022      //< LFQ队列中该区域为坏区域/
#define ERCD_HAP_STM_LFQ_UNEXPECTED                     0x10080023      //< LFQ队列中执行到不可能进入的逻辑/
#define ERCD_HAP_STM_HANDLE_INIT_FAILED                 0x10080024      //< LFQ队列handle初始化失败/
#define ERCD_HAP_STM_LFQ_MEM_TOO_LARGE                  0x10080025      //< LFQ队列内存分配过大/


#define ERCD_NOTOK                      0
#define NO_ERR          1
#define  TRUE           1
#define  FALSE          0

#if ! defined(CACHE_ALIGN_SIZE)
#   define  CACHE_ALIGN_SIZE 128

#if defined (__GNUC__)      // gcc with -traditional /
#   define  CACHE_ALIGN __attribute__((aligned(CACHE_ALIGN_SIZE)))
#else
#   define  CACHE_ALIGN
#endif

#endif

#if defined (__GNUC__)      // gcc with -traditional /
#   define  NO_INSTRUMENT __attribute__((__no_instrument_function__))
#   define  NO_INSTRUMENT_INLINE inline __attribute__((__no_instrument_function__, always_inline))
#else
#   define  NO_INSTRUMENT
#endif

// 判断是否出现错误 /
#define NOTOK(_rc_) ((_rc_) != NO_ERR)

#define AO_t unsigned long
// 无锁队列类型 /
typedef enum
{
    // 多写一读 /
    MWRITE_ONEREAD,
    // 一写一读 /
    ONEWRITE_ONEREAD,
    // 一写多读 /
    ONEWRITE_MREAD
} StmLfqTypeT;

// 队列的配置信息
typedef struct StmLfqConfigTag
{
    // 队列元素个数 
    CACHE_ALIGN int32       queueLen;
    // 元素大小 
    CACHE_ALIGN int32       elemSize;
    // 队列类型 
    CACHE_ALIGN StmLfqTypeT type;
} StmLfqConfigT;

// 内部数据结构 
typedef struct StmLfqTag
{
    CACHE_ALIGN int  lfqFd;
    // 下一个可读位置 
    CACHE_ALIGN volatile AO_t nxtRead;
    // 下一个可写位置 
    CACHE_ALIGN volatile AO_t nxtWrt;
    // 读方是否处于工作状态(仅在一读情况下有意义) 
    CACHE_ALIGN volatile AO_t isReaderWorking;
    // 队列配置 
    CACHE_ALIGN StmLfqConfigT config;
    // 队列数据区起始地址
    CACHE_ALIGN char  data[];

} StmLfqT;

typedef struct StmLfqHandleTag StmLfqHandleT;
typedef ResCodeT (StmLfqUserPushCB)(char * pDataAddr, char * pData, int32 size);
typedef ResCodeT (StmLfqUserPopCB)(char * pData, void * pUserData, int32 size);

// 计算无锁队列所占内存大小
ResCodeT StmLfqCalcSize(StmLfqConfigT* pConfig, int32* memSize);
// int32内存大小，可能达不到要求，特此增加接口扩充支持到uint64
ResCodeT StmLfqCalcSize1(StmLfqConfigT* pConfig, uint64* memSize);
//初始化用户分配的内存区域
ResCodeT StmLfqInit(StmLfqT* pLfqueue, StmLfqConfigT* pConfig);
//初始化无锁队列句柄
ResCodeT StmLfqInitHandle(StmLfqT* pLfqueue, struct StmLfqHandleTag** ppStmLfqHnd);
//向无锁队列压入数据
ResCodeT StmLfqPush(struct StmLfqHandleTag* pStmLfqHnd, StmLfqUserPushCB fp, char* pData, int32 size, BOOL* needTrigger);
//从无锁队列读取数据
ResCodeT StmLfqPop(struct StmLfqHandleTag* pStmLfqHnd, StmLfqUserPopCB fp, void* pUserData, int32 size);
//该接口用于向无锁队列批量压入数据，仅对一写情况可用
ResCodeT StmLfqBatPush(struct StmLfqHandleTag* pStmLfqHnd, StmLfqUserPushCB fp, char** pData, int32* size, int32 dataItemCnt, BOOL* needTrigger);
//设定读取位置
ResCodeT StmLfqSetReadPos(struct StmLfqHandleTag* pStmLfqHnd, uint64 readPos);
//销毁无锁队列句柄
ResCodeT StmLfqDestroyHandle(struct StmLfqHandleTag* pStmLfqHnd);
//获取队列已使用大小，不精确
uint64 StmLfqUsedSize(struct StmLfqHandleTag *pStmLfqHnd);
//获取队列剩余空闲大小，不精确
uint64 StmLfqFreeSize(struct StmLfqHandleTag *pStmLfqHnd);
//获取队列的文件操作符
ResCodeT StmLfqGetFd(struct StmLfqHandleTag* pStmLfqHnd, int32* pOutFd);

#ifdef __cplusplus
}
#endif

#endif  // _HAP_STM_LFQ_H /

