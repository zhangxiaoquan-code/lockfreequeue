#include "stm_lfq.h"
#include <pthread.h>
#include <sys/time.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#define  WRITER          10
#define SHM_MASK_ID      0666

typedef struct data
{
   int a;
   int b; 
}data;

ResCodeT LfqSlotPopCB(char *pQueueItemAddr, void *pLocalMsgAddr, int size)
{
    memcpy(pLocalMsgAddr, pQueueItemAddr, size);
    return NO_ERR;
}

ResCodeT LfqSlotPushCB(char *pQueueItemAddr, char *pLocalMsgAddr, int32 size)
{
    memcpy(pQueueItemAddr, pLocalMsgAddr, size);
    return NO_ERR;
}

ResCodeT InitLfq(StmLfqConfigT config,StmLfqHandleTag** ppHandle)
{
    int             rc = NO_ERR;
    uint64           memSize;
    StmLfqT*            pLfq= NULL;
    StmLfqHandleTag*    pHandle = NULL;
    key_t          key = ftok("./stm_lfq.cpp",1);
    int64 shmId = -1;
    
    rc = StmLfqCalcSize1(&config, &memSize);
    
    shmId =  shmget(key,memSize, SHM_MASK_ID|IPC_CREAT|IPC_EXCL);
    if (shmId == -1)
    {
        fprintf(stderr,"The reason of failed to create share memory:errno=%d,error=%s",errno,strerror(errno));
        return errno;    
    }
    pLfq = (StmLfqT*)shmat(shmId, (void *)0, 0);
    if (pLfq == NULL)
    {
        fprintf(stderr,"The reason of failed to create share memory:errno=%d,error=%s",errno,strerror(errno));
        return errno;    
    }
    rc = StmLfqInit(pLfq, &config);
    if (rc != 1)
    {
        fprintf(stderr,"****StmLfqInit error: %d\n", rc);
    }
    else
    {
        fprintf(stdout,"****StmLfqInit OK!\n");
    }

    rc = StmLfqInitHandle(pLfq, &pHandle);
    if (rc != 1)
    {
        fprintf(stderr,"****StmLfqInitHandle error: %d\n", rc);
    }
    else
    {
        fprintf(stdout,"****StmLfqInitHandle OK!\n");
    }
    
    *ppHandle = pHandle;

    return NO_ERR;
}

void * push(void* arg)
{
    data d;
    StmLfqHandleTag*    pHandle = (StmLfqHandleTag*)arg;
    int n = 0;
    int             rc = NO_ERR;
    BOOL            needTrigger = FALSE;
    static int count = 0;
    struct timeval t[2];
    gettimeofday(&t[0],NULL);
    d.a = n;
    d.b = n + 1;
    while(1)
    {   
        rc = StmLfqPush(pHandle, LfqSlotPushCB, (char*)&d, sizeof(data), &needTrigger);
        if (rc != 1)
        {
            fprintf(stderr,"****StmLfqPush error: %d\n", rc);
            usleep(10);
            continue;
        }
        n++;
        d.a = n;
        d.b = n + 1;
        
        if ( n == 30000000)
        {
            count ++ ; 
            //fprintf(stderr,"****pthread[%d] StmLfqPush finish\n", count);
            break;
        }
    }
    gettimeofday(&t[1],NULL);
    double u = (double)t[1].tv_sec +(double)t[1].tv_usec/1000000 - ((double)t[0].tv_sec +(double)t[0].tv_usec/1000000);
    
    fprintf(stdout,"Push[%d] time = %lfs,Push count %d\n", count,u,n);
    return NULL;
}

void * pop(void* arg)
{
    data d;
    StmLfqHandleTag*    pHandle = (StmLfqHandleTag*)arg;
    int n = 0;
    int             rc = NO_ERR;
    struct timeval t[2];
    gettimeofday(&t[0],NULL);
    while(1)
    {   
        rc = StmLfqPop(pHandle, LfqSlotPopCB, (char*)&d, sizeof(data));
        if (rc != 1)
        {
            fprintf(stderr,"****StmLfqPop error: %d\n", rc);
            usleep(10);
            continue;
        }
        
        if ( d.a + 1 != d.b)
            fprintf(stderr,"****StmLfqPop data: %d-%d\n", d.a , d.b);
        n++;
        if ( n == 30000000)
            break;
    }
    gettimeofday(&t[1],NULL);
    double u = (double)t[1].tv_sec +(double)t[1].tv_usec/1000000 - ((double)t[0].tv_sec +(double)t[0].tv_usec/1000000);
    
    fprintf(stdout,"Pop time = %lfs,Pop count %d\n", u,n);
    
    return NULL;
}

int main()
{
    int    rc = NO_ERR;
    struct timeval t[2];
    pthread_t writer;
    pthread_t reader;
    StmLfqConfigT   config;
    
    StmLfqHandleTag*    pHandle = NULL;
    
    config.elemSize = sizeof(data);
    config.queueLen = 1048576;       // 1048576  2097152
    config.type = ONEWRITE_ONEREAD;
    
    rc = InitLfq(config,&pHandle);
    if ( rc != NO_ERR)
    {
        fprintf(stdout,"******lock-free queue init error\n");
    }
    
    gettimeofday(&t[0],NULL);
    
    pthread_create(&writer,NULL,push,(void *)pHandle);
    pthread_create(&reader,NULL,pop,(void *)pHandle);
    

    pthread_join(writer,NULL);
    pthread_join(reader,NULL);
    
    gettimeofday(&t[1],NULL);
    double u = (double)t[1].tv_sec +(double)t[1].tv_usec/1000000 - ((double)t[0].tv_sec +(double)t[0].tv_usec/1000000);
    
    fprintf(stdout,"time = %lfs,%lf/s\n", u,30000000/u);
    
    return 0;
}