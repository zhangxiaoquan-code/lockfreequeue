/**
* @file    stm_lfq.c
*
* @since   
* @author  
*/
#include "lock_free_queue"
#include <time.h>

#define ADDRESS_ADD_OFFSET(_prt_, _offset_) \
                ((uint64)(_prt_) + (uint64)(_offset_))
#define ADDRESS_MINUS_OFFSET(_prt_, _offset_) \
                ((uint64)(_prt_) - (uint64)(_offset_))

#define SLOT_STATE_EMPTY    0
#define SLOT_STATE_USED     1
#define SLOT_STATE_BAD      2

#define LOOP_TIMES          1000


//the following para is used by eventfd and writev()

static const struct timespec popWaitTime_ = {0, 100};

typedef volatile AO_t StmLfqSlotStateT;
static __thread struct timespec sIpcPushWaitTime = {0,1000};

typedef ResCodeT (*StmLfqPushFunc)(struct StmLfqHandleTag *pStmLfqHnd,StmLfqUserPushCB fp, char *pData, int32 size, BOOL *needTrigger);
typedef ResCodeT (*StmLfqPopFunc)(struct StmLfqHandleTag *pStmLfqHnd,StmLfqUserPopCB fp, void *pUserData, int32 size);
typedef ResCodeT (*StmLfqBatPushFunc)(struct StmLfqHandleTag *pStmLfqHnd,StmLfqUserPushCB fp, char **pData, int32 *size, int32 dataItemCnt,BOOL *needTrigger);
typedef ResCodeT (*StmLfqSetReadPosFunc)(struct StmLfqHandleTag *pStmLfqHnd,uint64 readPos);

//MW1R
static ResCodeT NO_INSTRUMENT StmLfqPushMW1R(struct StmLfqHandleTag *pStmLfqHnd,StmLfqUserPushCB fp, char *pData, int32 size, BOOL *needTrigger);
static ResCodeT NO_INSTRUMENT StmLfqPopMW1R(struct StmLfqHandleTag *pStmLfqHnd,StmLfqUserPopCB fp, void *pUserData, int32 size);

//1W1R
static ResCodeT NO_INSTRUMENT StmLfqPush1W1R(struct StmLfqHandleTag *pStmLfqHnd,StmLfqUserPushCB fp, char *pData, int32 size, BOOL *needTrigger);
static ResCodeT NO_INSTRUMENT StmLfqPop1W1R(struct StmLfqHandleTag *pStmLfqHnd,StmLfqUserPopCB fp, void *pUserData, int32 size);

static ResCodeT StmLfqWriteData(char *pDataAddr, char *pData, int32 size);

static inline int __attribute__((__no_instrument_function__, always_inline))
cas(volatile AO_t *addr, AO_t old, AO_t new_val);
static inline int __attribute__((always_inline))
cas(volatile AO_t *addr, AO_t old, AO_t new_val) 
{
    char result;
    __asm__ __volatile__("lock; cmpxchgq %3, %0; setz %1"
                    : "=m"(*addr), "=q"(result)
                    : "m"(*addr), "r" (new_val), "a"(old) : "memory");
        return (int) result;
}

//内部数据结构 
struct StmLfqHandleTag
{
    //当前的读操作的位置
    CACHE_ALIGN volatile AO_t readPos;
    //当前的写操作的位置
    CACHE_ALIGN volatile AO_t wrtPos;
    //下一个可读位置
    CACHE_ALIGN uint64 l_nxtRead;
    //下一个可写位置
    CACHE_ALIGN uint64 l_nxtWrt;
    //队列的长度
    int32       l_queueLen;
    //队列的元素的大小
    int32       l_elemSize;
    char        *l_data;
    //队列的配置信息
    StmLfqT *pLfqueue;
    //队列的写操作
    StmLfqPushFunc push;
    //队列的读操作
    StmLfqPopFunc pop;
    //队列的批量写操作
    StmLfqBatPushFunc batPush;
    //队列的读位置的设置
    StmLfqSetReadPosFunc setReadPos;
};

/******************************************************************************
 * Description:   StmLfqCalcSize
 * Parameters:
 *          pConfig        IN     configureT
 *          memSize        IN     memSize
 * RETURN
 *      NO_ERR:     Successful
 *      ERR_<DSCR>: fail to create the share memory.
 ******************************************************************************/
ResCodeT StmLfqCalcSize(StmLfqConfigT* pConfig, int32* memSize)
{
    if (!pConfig || !memSize)
    {
        return ERCD_NOTOK;
    }
    int32 elemAlignSize = (pConfig->elemSize/16 + (pConfig->elemSize%16?1:0))*16;

    uint64 allSize = 0;
    switch (pConfig->type)
    {
        case MWRITE_ONEREAD:
            allSize = sizeof(StmLfqT) + (elemAlignSize + sizeof(StmLfqSlotStateT)) * pConfig->queueLen;
            break;
        case ONEWRITE_MREAD:
        case ONEWRITE_ONEREAD:
            allSize = sizeof(StmLfqT) + elemAlignSize * pConfig->queueLen;
            break;
    }
//    if (allSize > INT_MAX)
//        return ERCD_HAP_STM_LFQ_MEM_TOO_LARGE;

    *memSize = (int32)allSize;
    return NO_ERR;
}


/******************************************************************************
 * Description:   StmLfqCalcSize1
 * Parameters:
 *          pConfig        IN     configureT
 *          memSize        IN     memSize
 * RETURN
 *      NO_ERR:     Successful
 *      ERR_<DSCR>: fail to create the share memory.
 ******************************************************************************/
ResCodeT StmLfqCalcSize1(StmLfqConfigT* pConfig, uint64* memSize)
{
    if (!pConfig || !memSize)
    {
        return ERCD_NOTOK;
    }

    int32 elemAlignSize = (pConfig->elemSize/16 + (pConfig->elemSize%16?1:0))*16;

    switch (pConfig->type)
    {
        case MWRITE_ONEREAD:
            *memSize = sizeof(StmLfqT) + (elemAlignSize + sizeof(StmLfqSlotStateT)) * pConfig->queueLen;
            break;
        case ONEWRITE_MREAD:
        case ONEWRITE_ONEREAD:
            *memSize = sizeof(StmLfqT) + elemAlignSize * pConfig->queueLen;
            break;
    }

    return NO_ERR;
}

/******************************************************************************
 * Description:   ModefityStmLfqLen
 * Parameters:
 *          queueLen           IN      Not 2-power queue len
 *          l_queueLen         OUT     fix queue len
 * RETURN
 *      NO_ERR:     Successful
 ******************************************************************************/
ResCodeT ModStmLfqLen(int32 queueLen, int32 * l_queueLen)
{
    unsigned long maxULong = (unsigned long )((unsigned long)~0);
    unsigned long tmp     = ~(maxULong & (maxULong >> 1));
    while( (tmp & queueLen) == 0)
        tmp = tmp >>1;
        
    *l_queueLen = tmp << 1;
    return NO_ERR;
}


/******************************************************************************
 * Description:   StmLfqInit
 * Parameters:
 *          pLfqueue        IN     pLfqueue
 *          pConfig         IN     pConfig
 * RETURN
 *      NO_ERR:     Successful
 *      ERR_<DSCR>: fail to create the share memory.
 ******************************************************************************/
ResCodeT StmLfqInit(StmLfqT* pLfqueue, StmLfqConfigT* pConfig)
{
    if (!pLfqueue || !pConfig)
    {
        return ERCD_NOTOK;
    }

    if (pConfig->queueLen & (pConfig->queueLen - 1))
    {
        ModStmLfqLen(pConfig->queueLen,&(pConfig->queueLen));
    }

    uint64 memSize;
    StmLfqCalcSize1(pConfig, &memSize);

    memset(pLfqueue, 0, memSize);

    pLfqueue->config.elemSize = (pConfig->elemSize/16 + (pConfig->elemSize%16?1:0))*16;
    pLfqueue->config.queueLen = pConfig->queueLen;
    pLfqueue->config.type = pConfig->type;

    return NO_ERR;
}

/******************************************************************************
 * Description:   StmLfqInitHandle
 * Parameters:
 *          pLfqueue            IN     pLfqueue
 *          ppStmLfqHnd         IN     ppStmLfqHnd
 * RETURN
 *      NO_ERR:     Successful
 *      ERR_<DSCR>: fail to create the share memory.
 ******************************************************************************/
ResCodeT StmLfqInitHandle(StmLfqT* pLfqueue, struct StmLfqHandleTag** ppStmLfqHnd)
{
    if (!pLfqueue || !ppStmLfqHnd)
    {
        return ERCD_NOTOK;
    }
    StmLfqHandleT* pStmLfqHnd = (StmLfqHandleT*) malloc(sizeof(StmLfqHandleT));
    memset(pStmLfqHnd, 0, sizeof(StmLfqHandleT));

    pStmLfqHnd->pLfqueue = pLfqueue;
    pStmLfqHnd->l_nxtRead = pLfqueue->nxtRead;
    pStmLfqHnd->l_nxtWrt = pLfqueue->nxtWrt;
    pStmLfqHnd->l_queueLen = pLfqueue->config.queueLen -1;
    pStmLfqHnd->l_elemSize = pLfqueue->config.elemSize;
    pStmLfqHnd->l_data = pLfqueue->data;

    *ppStmLfqHnd = pStmLfqHnd;

    switch (pLfqueue->config.type)
    {
        case MWRITE_ONEREAD:
            (*ppStmLfqHnd)->push = StmLfqPushMW1R;
            (*ppStmLfqHnd)->pop = StmLfqPopMW1R;
            (*ppStmLfqHnd)->batPush = NULL;
            (*ppStmLfqHnd)->setReadPos = NULL;
            break;
        case ONEWRITE_MREAD:
           
            break;
        case ONEWRITE_ONEREAD:
            (*ppStmLfqHnd)->push = StmLfqPush1W1R;
            (*ppStmLfqHnd)->pop = StmLfqPop1W1R;
            //(*ppStmLfqHnd)->batPush = StmLfqBatPush1W1R;
            (*ppStmLfqHnd)->setReadPos = NULL;
            break;
        default:
            return ERCD_NOTOK;
            break;
    }
    return NO_ERR;
}


/******************************************************************************
 * Description:   StmLfqGetFd
 * Parameters:
 *          pStmLfqHnd     IN     pStmLfqHnd
 *          pOutFd         OUT     pOutFd
 * RETURN
 *      NO_ERR:     Successful
 *      ERR_<DSCR>: fail to create the share memory.
 ******************************************************************************/
ResCodeT StmLfqGetFd(struct StmLfqHandleTag* pStmLfqHnd, int32* pOutFd)
{
    * pOutFd = pStmLfqHnd->pLfqueue->lfqFd;
    return NO_ERR;
}

/******************************************************************************
 * Description:   StmLfqPush
 * Parameters:
 *          pStmLfqHnd    IN     pStmLfqHnd
 *          fp            IN     fp
 *          pData         IN     pData
 *          size          IN     size
 *         needTrigger   OUT     needTrigger
 * RETURN
 *      NO_ERR:     Successful
 *      ERR_<DSCR>: fail to create the share memory.
 ******************************************************************************/
ResCodeT StmLfqPush(struct StmLfqHandleTag* pStmLfqHnd, StmLfqUserPushCB fp, char* pData, int32 size, BOOL* needTrigger)
{
    if (!pStmLfqHnd || !pData || !size || !needTrigger)
    {
        return ERCD_HAP_STM_LFQ_ARG;
    }

    if (!fp)
    {
        fp = StmLfqWriteData;
    }
    return (*pStmLfqHnd->push)(pStmLfqHnd, fp, pData, size, needTrigger);
}

/******************************************************************************
 * Description:   StmLfqPop
 * Parameters:
 *          pStmLfqHnd    IN     pStmLfqHnd
 *          fp            IN     fp
 *          pData         IN     pUserData
 *          size          IN     size
 * RETURN
 *      NO_ERR:     Successful
 *      ERR_<DSCR>: fail to create the share memory.
 ******************************************************************************/
ResCodeT StmLfqPop(struct StmLfqHandleTag* pStmLfqHnd, StmLfqUserPopCB fp, void* pUserData, int32 size)
{
    if (!pStmLfqHnd || !fp)
    {
        return ERCD_HAP_STM_LFQ_ARG;
    }
    return (*pStmLfqHnd->pop)(pStmLfqHnd, fp, pUserData, size);
}

/******************************************************************************
 * Description:   StmLfqBatPush
 * Parameters:
 *          pStmLfqHnd    IN     pStmLfqHnd
 *          fp            IN     fp
 *          pData         IN     pUserData
 *          size          OUT    size
  *         dataItemCnt   IN     dataItemCnt
 *         needTrigger    OUT    needTrigger
 * RETURN
 *      NO_ERR:     Successful
 *      ERR_<DSCR>: fail to create the share memory.
 ******************************************************************************/
ResCodeT StmLfqBatPush(struct StmLfqHandleTag* pStmLfqHnd, StmLfqUserPushCB fp, char** pData, int32* size, int32 dataItemCnt, BOOL* needTrigger)
{
    if (!pStmLfqHnd || !pData || !size || !needTrigger)
    {
        return ERCD_HAP_STM_LFQ_ARG;
    }

    if (!pStmLfqHnd->batPush)
    {
        return ERCD_HAP_STM_LFQ_METHOD_NOT_SUPPORT;
    }

    if (!fp)
    {
        fp = StmLfqWriteData;
    }
    return (*pStmLfqHnd->batPush)(pStmLfqHnd, fp, pData, size, dataItemCnt,
            needTrigger);
}

/******************************************************************************
 * Description:   StmLfqSetReadPos
 * Parameters:
 *          pStmLfqHnd    IN     pStmLfqHnd
 *          readPos       IN     readPos
 * RETURN
 *      NO_ERR:     Successful
 *      ERR_<DSCR>: fail to create the share memory.
 ******************************************************************************/
ResCodeT StmLfqSetReadPos(struct StmLfqHandleTag* pStmLfqHnd, uint64 readPos)
{
    if (!pStmLfqHnd)
    {
        return ERCD_HAP_STM_LFQ_ARG;
    }

    if (!pStmLfqHnd->setReadPos)
    {
        return ERCD_HAP_STM_LFQ_METHOD_NOT_SUPPORT;
    }

    return (*pStmLfqHnd->setReadPos)(pStmLfqHnd, readPos);
}

/******************************************************************************
 * Description:   StmLfqDestroyHandle
 * Parameters:
 *      pStmLfqHnd    IN     pStmLfqHnd
 * RETURN
 *      NO_ERR:     Successful
 *      ERR_<DSCR>: fail to create the share memory.
 ******************************************************************************/
ResCodeT StmLfqDestroyHandle(struct StmLfqHandleTag* pStmLfqHnd)
{
    //printf("StmLfqDestroyHandle:%p\n", pStmLfqHnd);
    if (!pStmLfqHnd)
    {
        return ERCD_NOTOK;
    }
    free(pStmLfqHnd);
    return NO_ERR;
}

static ResCodeT StmLfqWriteData(char *pDataAddr, char *pData, int32 size)
{
    switch (size)
    {
        //write int32 to queue
        case 4:
            *((int32*) pDataAddr) = *((int32*) pData);
            break;
        //write int64 to queue
        case 8:
            *((int64*) pDataAddr) = *((int64*) pData);
            break;
        //write (int32 + int64) to queue
        case 12:
            *((int64*) pDataAddr) = *((int64*) pData);
            *((int32*) ADDRESS_ADD_OFFSET(pDataAddr, 8))
                    = *((int32*) ADDRESS_ADD_OFFSET(pData, 8));
            break;
        //write (int64 + int64 ) to queue
        case 16:
            *((int64*) pDataAddr) = *((int64*) pData);
            *((int64*) ADDRESS_ADD_OFFSET(pDataAddr, 8))
                    = *((int64*) ADDRESS_ADD_OFFSET(pData, 8));
            break;
        //write other-type data to queue
        default:
            memcpy(pDataAddr, pData, size);
            break;
    }
    return NO_ERR;
}

/******************************************************************************
 * Description:   StmLfqPushMW1R
 * Parameters:
 *      pStmLfqHnd    IN     pStmLfqHnd
 *      fp            IN     lock-free queue callback
 *      pData         IN     data need to write lock-free data
 *      size          IN     data size
 *      needTrigger   IN     Trigger
 * RETURN
 *      NO_ERR:     Successful
 *      ERR_<DSCR>: fail to push lock-free queue .
 ******************************************************************************/
static ResCodeT StmLfqPushMW1R(struct StmLfqHandleTag *pStmLfqHnd,StmLfqUserPushCB fp, char *pData, int32 size, BOOL *needTrigger)
{
    ResCodeT rc;
    register int32 pos;
    uint64 offset;
    volatile StmLfqSlotStateT *pSlotState;
    register char *pDataAddr;
    register uint64 queueLen = pStmLfqHnd->l_queueLen;
    register uint64 localWriteCursor;
    
again:    
    localWriteCursor = pStmLfqHnd->pLfqueue->nxtWrt;
    //queue full:pStmLfqHnd->pLfqueue->nxtWrt - queueLen + pStmLfqHnd->l_nxtRead > queueLen
    if (localWriteCursor > queueLen + pStmLfqHnd->l_nxtRead){
            pStmLfqHnd->l_nxtRead = pStmLfqHnd->pLfqueue->nxtRead;
    
        if (localWriteCursor > queueLen + pStmLfqHnd->l_nxtRead)
        {
            *needTrigger = FALSE;
            return ERCD_HAP_STM_LFQ_TRY_AGAIN;
        } 
    }
    //确定此时可用的槽位，将可用的槽位信息更新，提高无锁队列的使用效率
    if (!cas(&pStmLfqHnd->pLfqueue->nxtWrt, localWriteCursor,localWriteCursor + 1)){
        nanosleep(&sIpcPushWaitTime,NULL);
        goto again;
    }
           
    pos = localWriteCursor & queueLen;
    offset = (pStmLfqHnd->l_elemSize + sizeof(StmLfqSlotStateT)) * pos;
    pSlotState = (StmLfqSlotStateT *) ADDRESS_ADD_OFFSET(pStmLfqHnd->l_data, offset);

    switch (*pSlotState)
    {
        //空槽位才可被写入内容
        case SLOT_STATE_EMPTY:
            pDataAddr = (char *) ADDRESS_ADD_OFFSET( pSlotState, sizeof(StmLfqSlotStateT));
            rc = fp(pDataAddr, pData, size);
            if (NOTOK(rc))
            {
                return rc;
            }
            //槽位写入内容后，更改槽位的状态
            if (!cas(pSlotState, SLOT_STATE_EMPTY, SLOT_STATE_USED))
            {
                *needTrigger = FALSE;
                return ERCD_HAP_STM_LFQ_WRITE_TIMEOUT;
            }
             //如有读操作等待，通知操作，有槽位可被读取
            *needTrigger = cas( &(pStmLfqHnd->pLfqueue->isReaderWorking), FALSE, TRUE);
    
            return NO_ERR;            
        case SLOT_STATE_USED:
            *needTrigger = FALSE;
            return ERCD_HAP_STM_LFQ_INVALID_POS;        
        case SLOT_STATE_BAD:
            goto again;        
        default:
            *needTrigger = FALSE;
            return ERCD_NOTOK;
    }
    
    //while (write(pStmLfqHnd->pLfqueue->lfqFd,&eventfdDelta,sizeof(eventfdDelta))
      //  < (int)sizeof(eventfdDelta));

}
/******************************************************************************
 * Description:   StmLfqPushMW1R
 * Parameters:
 *      pStmLfqHnd    IN     pStmLfqHnd
 *      fp            IN     lock-free queue callback
 *      pData         IN     data need to read from lock-free data
 *      size          IN     data size
 * RETURN
 *      NO_ERR:     Successful
 *      ERR_<DSCR>: fail to read lock-free queue.
 ******************************************************************************/
static ResCodeT StmLfqPopMW1R(struct StmLfqHandleTag *pStmLfqHnd,StmLfqUserPopCB fp, void *pUserData,int32 size)
{
    ResCodeT rc;
    register int32 readPosition;
    register int32 loop = LOOP_TIMES;
    register char *readAddress;
    volatile StmLfqSlotStateT* pSlotState;
    register uint64 localReadCursor;
   
again:
    localReadCursor = pStmLfqHnd->pLfqueue->nxtRead;
    //是否有可读的数据:(pStmLfqHnd->pLfqueue->nxtRead) > (pStmLfqHnd->pLfqueue->nxtWrt) 
    if (localReadCursor >= pStmLfqHnd->l_nxtWrt){
        pStmLfqHnd->l_nxtWrt = pStmLfqHnd->pLfqueue->nxtWrt;
        if (localReadCursor >= pStmLfqHnd->l_nxtWrt){
            if (pStmLfqHnd->pLfqueue->isReaderWorking &&!cas( &(pStmLfqHnd->pLfqueue->isReaderWorking), TRUE, FALSE))
            {
                goto again;
            }
            return ERCD_HAP_STM_LFQ_TRY_AGAIN;        
        }
    }
    
    readPosition = localReadCursor & (pStmLfqHnd->l_queueLen);
    readAddress = (char *) ADDRESS_ADD_OFFSET(pStmLfqHnd->l_data,(sizeof(StmLfqSlotStateT) + pStmLfqHnd->l_elemSize) * readPosition);
    pSlotState = (StmLfqSlotStateT*)readAddress;
    
    if (SLOT_STATE_BAD == *pSlotState)
    {        
        pStmLfqHnd->pLfqueue->nxtRead++;
        goto again;
    } 
    //当前的可读槽位为空，等待LOOP_TIMES 
    while (SLOT_STATE_EMPTY == *pSlotState && --loop)
        nanosleep(&popWaitTime_,NULL);
    //等待超时，当前的可读槽位依然为空，将该槽位设为SLOT_STATE_BAD状态
    if (!loop && cas(pSlotState, SLOT_STATE_EMPTY, SLOT_STATE_BAD)){
            pStmLfqHnd->pLfqueue->nxtRead++;
            return ERCD_HAP_STM_LFQ_BAD_SLOT;
    }

    rc = fp((char *) ADDRESS_ADD_OFFSET(readAddress, sizeof(StmLfqSlotStateT)), pUserData, size);
    if (NOTOK(rc))    
        return rc;
    //正常读取该槽位的数据后，将该槽位设为SLOT_STATE_EMPTY状态
    *pSlotState = SLOT_STATE_EMPTY; 
    pStmLfqHnd->pLfqueue->nxtRead++;
        
    return NO_ERR;
}
/******************************************************************************
 * Description:   StmLfqPush1W1R
 * Parameters:
 *      pStmLfqHnd    IN     pStmLfqHnd
 *      fp            IN     lock-free queue callback
 *      pData         IN     data need to write lock-free data
 *      size          IN     data size
 *      needTrigger   IN     Trigger
 * RETURN
 *      NO_ERR:     Successful
 *      ERR_<DSCR>: fail to push lock-free queue .
 ******************************************************************************/
static ResCodeT StmLfqPush1W1R(struct StmLfqHandleTag *pStmLfqHnd,StmLfqUserPushCB fp, char *pData, int32 size, BOOL *needTrigger)
{
    register ResCodeT rc;
    register int32 writePosition;
    register char *writeAddress;
    register int32 queueLen = pStmLfqHnd->l_queueLen;

    while (TRUE)
    {
        if ((pStmLfqHnd->l_nxtWrt + 1 - pStmLfqHnd->l_nxtRead) & queueLen)
        {
            writePosition = pStmLfqHnd->l_nxtWrt & queueLen;
            writeAddress = (char *) ADDRESS_ADD_OFFSET(pStmLfqHnd->l_data, pStmLfqHnd->l_elemSize * writePosition);
            rc = fp(writeAddress, pData, size);
            if (NOTOK(rc))
            {
                return rc;
            }
            pStmLfqHnd->pLfqueue->nxtWrt = ++pStmLfqHnd->l_nxtWrt;
    
            *needTrigger = cas(&(pStmLfqHnd->pLfqueue->isReaderWorking), FALSE, TRUE);
            break;
        } 
        else
        {
            pStmLfqHnd->l_nxtRead = pStmLfqHnd->pLfqueue->nxtRead;
            if (!((pStmLfqHnd->l_nxtWrt + 1 - pStmLfqHnd->l_nxtRead) & queueLen))
            {
                return ERCD_HAP_STM_LFQ_TRY_AGAIN;
            }
        }
    }
    return NO_ERR;
}
/******************************************************************************
 * Description:   StmLfqPop1W1R
 * Parameters:
 *      pStmLfqHnd    IN     pStmLfqHnd
 *      fp            IN     lock-free queue callback
 *      pData         IN     data need to read from lock-free data
 *      size          IN     data size
 * RETURN
 *      NO_ERR:     Successful
 *      ERR_<DSCR>: fail to read lock-free queue.
 ******************************************************************************/
static ResCodeT StmLfqPop1W1R(struct StmLfqHandleTag *pStmLfqHnd,
        StmLfqUserPopCB fp, void *pUserData,int32 size)
{
    register ResCodeT rc;
    register uint64 readPosition;
    register char *readAddress;
    register int32 queueLen = pStmLfqHnd->l_queueLen;

    while (TRUE)
    {
        if ((readPosition = pStmLfqHnd->l_nxtRead & queueLen)!= (pStmLfqHnd->l_nxtWrt & queueLen))
        {
            readAddress = (char *) ADDRESS_ADD_OFFSET(pStmLfqHnd->l_data,(pStmLfqHnd->l_elemSize) * readPosition);
            rc = fp(readAddress, pUserData,size);
            if (NOTOK(rc))
            {
                return rc;
            }
            pStmLfqHnd->pLfqueue->nxtRead = ++pStmLfqHnd->l_nxtRead;
            break;
        }
        else
        {
            pStmLfqHnd->l_nxtWrt = pStmLfqHnd->pLfqueue->nxtWrt;

            if (!((pStmLfqHnd->l_nxtWrt - pStmLfqHnd->l_nxtRead) & queueLen))
            {
                pStmLfqHnd->pLfqueue->isReaderWorking = FALSE;
                return ERCD_HAP_STM_LFQ_TRY_AGAIN;
            }
        }
    }
    return NO_ERR;
}
/******************************************************************************
 * Description:   StmLfqUsedSize
 * Parameters:
 *      pStmLfqHnd    IN     pStmLfqHnd
 * RETURN
 *      uint64:         used size lock-free size
 ******************************************************************************/
uint64 StmLfqUsedSize(struct StmLfqHandleTag *pStmLfqHnd)
{
    StmLfqT *pLfqueue = pStmLfqHnd->pLfqueue;
    uint64 ret = pLfqueue->nxtRead;
    __sync_synchronize();
    return  pLfqueue->nxtWrt -ret;
}
/******************************************************************************
 * Description:   StmLfqFreeSize
 * Parameters:
 *      pStmLfqHnd    IN     pStmLfqHnd
 * RETURN
 *      uint64:         free size lock-free size
 ******************************************************************************/
uint64 StmLfqFreeSize(struct StmLfqHandleTag *pStmLfqHnd)
{
    StmLfqT *pLfqueue = pStmLfqHnd->pLfqueue;
    uint64 used = pLfqueue->nxtWrt - pLfqueue->nxtRead;
    __sync_synchronize();
    return pStmLfqHnd->l_queueLen - used;
}

