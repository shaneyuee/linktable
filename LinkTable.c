/******************************************************************************

 FILENAME:	LinkTable.c

  DESCRIPTION:		数组索引+链表方式内存接口定义

 HISTORY:
           Date          Author         Comment
        2008-05-15      RunnerWang      Created
        2010-09-04      Forestli        支持64位uin
        2011-09-14      Shaneyu         对IndexHead进行排序和二分查找
        2011-11-30      Shaneyu         将空闲节点存放在Free链表，滚回32位key
        2012-05-01      Shaneyu         将固定大小的数组改成可配置大小的块
        2012-09-13      Shaneyu         增加预回收池，防止读写冲突
******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/mman.h>

#ifdef  __cplusplus
extern "C" {
#endif
    #include "common.h"
    #include "oi_shm.h"
    #include "oi_str2.h"
    #include "Attr_API.h"
    #include "oi_debug.h"
#ifdef  __cplusplus
}
#endif

#include "LinkTableDefine.h"
#include "LinkTable.h"

#ifdef ELEMENT_DATABUF_LEN
#undef ELEMENT_DATABUF_LEN
#endif
#define ELEMENT_DATABUF_LEN	(g_stTable.pstTableHead->dwElementSize - (unsigned long)(((Element*)0)->bufData))

volatile LinkTable g_stTable;

static int compare_indexid(const void *pa, const void *pb)
{
    const IndexHead *a, *b;

    a = (const IndexHead *)pa;
    b = (const IndexHead *)pb;

    return a->dwIndexId - b->dwIndexId;
}

static int InsertIndexHead(uint32_t dwIndexId, IndexHead *base,
        size_t nmemb, size_t nmaxmemb, IndexHead **ppPtr, int createFlag)
{
    IndexHead ih;
    IndexHead *pSearch = NULL;

    int iEqual = 0;

    if (dwIndexId == 0 ||
            base == NULL ||
            nmaxmemb == 0)
    {
        return -1;
    }

    ih.dwIndexId = dwIndexId;
    ih.iIndex = 0;

    pSearch = (IndexHead *)my_bsearch(&ih, base, nmemb, sizeof(IndexHead), &iEqual, compare_indexid);
    if (pSearch && iEqual)
    {
        if (ppPtr)
        {
            *ppPtr = pSearch;
        }
        return 0;
    }

    if(createFlag==0 ||
        // If IndexIds are loaded at init time, no further modification is allowed
        g_stTable.pstTableHead->bLockedIndexIds)
    {
        return -2;
    }

    if ( (nmaxmemb - nmemb) < 1)
    {
        return -2; //特殊的返回码，注意保持
    }

    if (pSearch)
    {
    	int iIndex = base[nmemb].iIndex;

        // move one element forwards
        memmove(pSearch+1, pSearch, ((unsigned long)(base+nmemb)) - ((unsigned long)pSearch));

        pSearch->iIndex = iIndex;
    }
    else
    {
        pSearch = base + nmemb;
    }

    pSearch->dwIndexId = dwIndexId;

    // Can index go insane?
    if(pSearch->iIndex<0 || (size_t)pSearch->iIndex>=nmaxmemb)
    {
        // BUG: insane index
        Attr_API(162204, 1);
        pSearch->iIndex = nmemb;
    }

    if(ppPtr)
        *ppPtr = pSearch;

    return 1;
}

static uint32_t CalShmSize(uint32_t dwIndexCount, uint32_t dwIndexRowCount, uint32_t dwElementCount, uint32_t dwElementSize)
{
    uint32_t dwShmSize=(sizeof(LinkTableHead)+sizeof(IndexHead)*dwIndexCount+
            sizeof(IndexNode)*dwIndexCount*dwIndexRowCount+dwElementSize*dwElementCount);

    return dwShmSize;
}

// 这里重写一下，支持unsigned size，不打印出错，不mlock
static void *LT_GetShm(int iKey, uint32_t dwSize, int iFlag)
{
    int iShmID;
    char *sShm;

    if ((iShmID = shmget(iKey, dwSize, iFlag)) < 0)
        return 0;

    if ((sShm = (char *)shmat(iShmID, NULL ,0)) == (char *) -1)
        return 0;

    return sShm;
}

static int CreateLinkTableShm(int iKey, uint32_t dwShmSize, uint32_t dwIndexCount, uint32_t dwIndexRowCount, uint32_t dwElementCount, uint32_t dwElementSize)
{
    uint32_t u;
    LinkTableHead *pTableHead;

    pTableHead = (LinkTableHead *)GetShm(iKey, dwShmSize, 0666|IPC_CREAT);
    if(pTableHead==NULL)
        return -2;

    memset(pTableHead, 0, dwShmSize);

    // The first element is reserved
    pTableHead->dwAllElementCount=dwElementCount-1;
    pTableHead->dwFreeElementCount=dwElementCount-1;
    pTableHead->dwElementSize=dwElementSize;
    pTableHead->dwIndexCount=dwIndexCount;
    pTableHead->dwIndexRowCount=dwIndexRowCount;
    pTableHead->dwLastEmpty=1;

    // 初始化索引值为自身所在的索引
    for(u=0; u<dwIndexCount; u++)
        ((IndexHead *)(pTableHead+1))[u].iIndex = u;

    shmdt(pTableHead);
    return 0;
}

static int VerifyLinkTableParam(void *pTable, uint32_t dwIndexCount, uint32_t dwIndexRowCount,
	    uint32_t dwElementCount, uint32_t dwElementSize)
{
    LinkTableHead *pTableHead;

    pTableHead = (LinkTableHead *)pTable;
    if((pTableHead->dwAllElementCount!=(dwElementCount-1)) ||
        (pTableHead->dwIndexCount!=dwIndexCount) ||
        (pTableHead->dwIndexRowCount!=dwIndexRowCount) ||
        (pTableHead->dwElementSize!=dwElementSize))
    {
        printf("Error: shm parameters mismatched: \n");
        printf("\tdwIndexCount: given %u, got %u\n", dwIndexCount, pTableHead->dwIndexCount);
        printf("\tdwIndexRwCnt: given %u, got %u\n", dwIndexRowCount, pTableHead->dwIndexRowCount);
        printf("\tdwElementCnt: given %u, got %u\n", dwElementCount, pTableHead->dwAllElementCount+1);
        printf("\tdwElementSz:  given %u, got %u\n", dwElementSize, pTableHead->dwElementSize);
        return -1;
    }

    return 0;
}

// 验证SHM中的参数与配置的参数是否一致，不一致则返回失败
// 返回值： 0 不存在  1 存在且参数合法  -1 参数错误
static int VerifyLinkTableSize(uint32_t dwIndexCount, uint32_t dwIndexRowCount,
            uint32_t dwElementCount, uint32_t dwElementSize, int iKey)
{
    void *pTable;
    int iRet;

    pTable = LT_GetShm(iKey, sizeof(LinkTable)+sizeof(LinkTableHead), 0666);
    if(pTable == 0) // shm does not exist
        return 0;

    iRet = VerifyLinkTableParam(pTable, dwIndexCount, dwIndexRowCount, dwElementCount, dwElementSize);
    shmdt(pTable);

    if(iRet<0)
        return -1;

    return 1;
}

static void RecycleAllPreFreeElements();

int InitLinkTable( uint32_t dwIndexCount, uint32_t dwIndexRowCount,
    uint32_t dwElementCount, uint32_t dwElementSize, int iKey,int iCreateFlag)
{
    char * pTable=NULL;
    uint32_t dwShmSize;
    int iVerifyResult;

    if(dwIndexCount<10 || dwIndexRowCount<100 || dwElementCount<1000 ||
       dwElementSize<10 || dwElementSize>MAX_ELEMENT_SIZE || (iCreateFlag!=0 && iCreateFlag!=1))
    {
        fprintf(stderr, "Invalid parameters: "
                "dwIndexCount=%u, dwIndexRowCount=%u, dwElementCount=%u, dwElementSize=%u(max=%u), iCreateFlag=%d\n",
                dwIndexCount, dwIndexRowCount, dwElementCount, dwElementSize, MAX_ELEMENT_SIZE, iCreateFlag);
        return -1;
    }

    dwShmSize=CalShmSize(dwIndexCount, dwIndexRowCount, dwElementCount, dwElementSize);
#if 0
    printf("dwIndexCount=%u, dwIndexRowCount=%u, dwElementCount=%u, dwElementSize=%u, shmsize=%uU=%dS\n",
    		dwIndexCount, dwIndexRowCount, dwElementCount, dwElementSize, dwShmSize, (int)dwShmSize);
#endif

    if(dwShmSize==0 || dwShmSize > MAX_SHM_SIZE)
    {
        fprintf(stderr, "Invalid shm size %u (max=%u)\n", dwShmSize, MAX_SHM_SIZE);
        return -2;
    }

    iVerifyResult = VerifyLinkTableSize(dwIndexCount, dwIndexRowCount, dwElementCount, dwElementSize, iKey);
    if(iVerifyResult < 0)
    {
        fprintf(stderr, "VerifyLinkTableSize() returns %d.\n", iVerifyResult);
        return -6;
    }

    if(iCreateFlag==1 && iVerifyResult==0)
    {
        if(CreateLinkTableShm(iKey, dwShmSize, dwIndexCount, dwIndexRowCount, dwElementCount, dwElementSize))
            return -3;
    }

    pTable=GetShm(iKey, dwShmSize, 0666);
    if(pTable==NULL)
    {
        fprintf(stderr, "Shm key=%u, size=%u does not exist.\n", iKey, dwShmSize);
        return -4;
    }

    if(mlock(pTable, dwShmSize)!=0)
    {
        perror("mlock fail");
        return -7;
    }

    g_stTable.pstTableHead=(LinkTableHead*)pTable;
    g_stTable.pstIndexHeadList=(IndexHead *)(pTable+sizeof(LinkTableHead));
    g_stTable.pstIndexNodeList=(IndexNode *)(pTable+sizeof(LinkTableHead)+sizeof(IndexHead)*dwIndexCount);
    g_stTable.pstElementList=(Element  *)(pTable+sizeof(LinkTableHead)+sizeof(IndexHead)*dwIndexCount+
        sizeof(IndexNode)*dwIndexCount*dwIndexRowCount);

    // Reverify anyway
    if(VerifyLinkTableParam(pTable, dwIndexCount, dwIndexRowCount, dwElementCount, dwElementSize))
        return -5;

    // 程序启动时将预回收池中的所有元素回收掉
    if(iCreateFlag==1)
        RecycleAllPreFreeElements();
    return 0;
}

// 如果pstIndexHeadList为空，则将LinkTable的pstIndexHeadList初始化为相应的UnitList
// 否则，比较pstIndexHeadList和dwUnitList，如果不相对应则返回失败
// dwUnitList必须是已经排好序的
// 成功返回0，否则返回负数
int SetLinkTableUnits(uint32_t dwUnitList[], int iUnitNum)
{
    int i;

    if(g_stTable.pstTableHead==NULL || dwUnitList==NULL ||
         iUnitNum<0 || iUnitNum>(int)g_stTable.pstTableHead->dwIndexCount)
        return -1;

    // 检查 dwUnitList是否已经排好序
    for(i=0; i<iUnitNum-1; i++)
    {
        if(dwUnitList[i]>=dwUnitList[i+1])
            return -2;
    }

    if(g_stTable.pstTableHead->dwUsedIndexCount>0) // IndexHeadList不为空
    {
        if((int)g_stTable.pstTableHead->dwUsedIndexCount!=iUnitNum)
            return -3;

        for(i=0; i<iUnitNum; i++)
        {
            // IndexId has changed
            if(g_stTable.pstIndexHeadList[i].dwIndexId != (dwUnitList[i]+1))
                return -4;

            // Index has changed
            if(g_stTable.pstIndexHeadList[i].iIndex != i)
                return -5;
        }
    }
    else // IndexHeadList为空
    {
        for(i=0; i<iUnitNum; i++)
        {
            g_stTable.pstIndexHeadList[i].iIndex = i;
            g_stTable.pstIndexHeadList[i].dwIndexId = dwUnitList[i]+1;
        }
        for(; i<(int)g_stTable.pstTableHead->dwIndexCount; i++)
        {
            g_stTable.pstIndexHeadList[i].iIndex = i;
            g_stTable.pstIndexHeadList[i].dwIndexId = 0;
        }
        g_stTable.pstTableHead->dwUsedIndexCount = iUnitNum;

        // From now on, pstIndexHeadList should not be changed
        g_stTable.pstTableHead->bLockedIndexIds = 1;
    }

    return 0;
}


static int GetIndexHead(uint32_t dwIndexId, IndexHead **ppih, int createFlag)
{
    int ret;

    if(ppih==NULL)
        return -1;

    if(g_stTable.pstTableHead->dwUsedIndexCount > g_stTable.pstTableHead->dwIndexCount)
    {
        Attr_API(162204, 1);
        return -2; // BUG: index goes insane
    }

    ret = InsertIndexHead(dwIndexId, g_stTable.pstIndexHeadList,
            g_stTable.pstTableHead->dwUsedIndexCount,
            g_stTable.pstTableHead->dwIndexCount, ppih, createFlag);

    if(ret<0 || *ppih==NULL)
        // createFlag=1: 没有空间了，需要马上扩容
        // createFlag=0: 找不到
    {
        return -3;
    }
    else if(ret==0) // 找到存在的了
    {
    }
    else // 找不到，增加新的
    {
        g_stTable.pstTableHead->dwUsedIndexCount ++;
    }
    return ret;
}

static int RecycleElement(uint32_t dwStartPos);

// 回收全部预回收池中的元素
static void RecycleAllPreFreeElements()
{
    int i;

    Attr_API(241908, 1);

    for(i=0; i<LT_PRE_FREE_POOL_SIZE; i++)
    {
        if(g_stTable.pstTableHead->adwPreFreePool[i])
        {
            int iRet;

            iRet = RecycleElement(g_stTable.pstTableHead->adwPreFreePool[i]);
            if(iRet<0)
            {
                // 如果回收失败，将导致单元不能被重新利用，这里只上报，不处理
                Attr_API(241914, 1);
            }

            g_stTable.pstTableHead->adwPreFreePool[i] = 0;
        }
    }

    g_stTable.pstTableHead->dwPreFreeIndex = 0;
}

// 预回收池的主要算法实现：
// 用一个游标dwPreFreeIndex循环遍历预回收池指针数组adwPreFreePool，没次预回收遍历一个指针，
// 将本次希望回收的起始地址挂到adwPreFreePool[dwPreFreeIndex]上，并回收上次挂到这的地方的元素链表
int PreRecycleElement(uint32_t dwStartPos)
{
    int iRet;
    uint32_t dwOldPos, dwIdx;

    Attr_API(241972, 1);

    dwIdx = g_stTable.pstTableHead->dwPreFreeIndex;
    if(dwIdx>=LT_PRE_FREE_POOL_SIZE)
    {
        Attr_API(241911, 1);
        dwIdx = 0;
        g_stTable.pstTableHead->dwPreFreeIndex = 0;
    }

    // 先放到预回收池中，再回收同个位置中的旧数据
    dwOldPos = g_stTable.pstTableHead->adwPreFreePool[dwIdx];
    g_stTable.pstTableHead->adwPreFreePool[dwIdx] = dwStartPos;
    g_stTable.pstTableHead->dwPreFreeIndex = (dwIdx+1)%LT_PRE_FREE_POOL_SIZE;

    if(dwOldPos)
    {
        iRet = RecycleElement(dwOldPos);
        if(iRet<0)
        {
            // 如果回收失败，将导致单元不能被重新利用，这里只上报，不处理
            Attr_API(241914, 1);
        }
    }

    return 0;
}


//使用数组方式管理自由空间，分配和使用自由空间采用步进方式
static int GetEmptyElement(int iCount, uint32_t *pdwStartPos)
{
    uint32_t  dwStartPos,dwCurPos;
    int i,j,iTrySecond=0;
    float fUsedRate;

    if(g_stTable.pstTableHead==NULL)
    {
        return -1;
    }

    // 只要空闲块数小于预设比例就告警
    fUsedRate = (g_stTable.pstTableHead->dwFreeElementCount*100.0/g_stTable.pstTableHead->dwAllElementCount);
    if(fUsedRate<20.0) Attr_API(178992, 1);
    if(fUsedRate<15.0) Attr_API(232399, 1);
    if(fUsedRate<10.0) Attr_API(241997, 1);
    if(fUsedRate<5.0)  Attr_API(242000, 1);

restart:
    if(iTrySecond) // 清空预回收池后重新分配
        Attr_API(242023, 1);

    if((iCount<0) ||(pdwStartPos==NULL) ||
        (iCount > MAX_BLOCK_COUNT) ||
        (iCount > (int)g_stTable.pstTableHead->dwFreeElementCount))
    {
        // Not enough space
        if(iTrySecond==0)
        {
            iTrySecond = 1;
            RecycleAllPreFreeElements();
            goto restart;
        }
        Attr_API(161647, 1);
        return -2;
    }

    // 先从Free列表搜索，搜不到再挨个搜索
    dwStartPos=0;
    dwCurPos=g_stTable.pstTableHead->dwFirstFreePos;
    i=0;
    while(i<iCount && dwCurPos>0 &&
          dwCurPos<g_stTable.pstTableHead->dwAllElementCount &&
          GET_ELE_AT(dwCurPos)->dwKey==0)
    {
        unsigned int dwNext;
        i++;
        dwNext = GET_ELE_AT(dwCurPos)->dwNext;
        if(i==iCount) // 把最后一个的Next置零
            GET_ELE_AT(dwCurPos)->dwNext = 0;
        dwCurPos=dwNext;
    }

    if(i==iCount) // 找到了
    {
        *pdwStartPos = g_stTable.pstTableHead->dwFirstFreePos;
        g_stTable.pstTableHead->dwFirstFreePos = dwCurPos;
        Attr_API(178921, 1);
        if(iTrySecond)
            Attr_API(242024, 1);
        return 0;
    }

    // FIXME：这里存在一个不是很重要的问题：就是当从Free链表中找到一些节点，但无法满足请求的需要，
    // 这时需要通过遍历搜索来分配，而遍历搜索可能会找到free链表中的节点，这将导致free链表中的节点被
    // 遍历分配了，使得Free链表断链，断链后的链表就只能由遍历分配器来分配了

    // 从pstTableHead->dwLastEmpty开始进行挨个搜索
    dwStartPos=0;
    dwCurPos=g_stTable.pstTableHead->dwLastEmpty;
    i=0;
    for(j=0; (i<iCount) && (j<(int)g_stTable.pstTableHead->dwAllElementCount);j++)
    {
        if(GET_ELE_AT(dwCurPos)->dwKey==0)
        {
            GET_ELE_AT(dwCurPos)->dwNext=dwStartPos;
            dwStartPos=dwCurPos;
            i++;
        }
        dwCurPos++;

        // Wrap around
        if(dwCurPos>=g_stTable.pstTableHead->dwAllElementCount)
        {
            dwCurPos=1;
        }
    }

    if(i<iCount)
    {
        // Not enough space
        if(iTrySecond==0)
        {
            iTrySecond = 1;
            RecycleAllPreFreeElements();
            goto restart;
        }
        Attr_API(161647, 1);
        return -4;
    }

    *pdwStartPos=dwStartPos;
    g_stTable.pstTableHead->dwLastEmpty=dwCurPos;
    Attr_API(178922,1);
    if(iTrySecond)
        Attr_API(242024, 1);
    return 0;
}

static int RecycleElement(uint32_t dwStartPos)
{
    uint32_t dwCurPos=0, dwNextPos=0;
    int iCount=0;

    Attr_API(241995, 1);

    if(g_stTable.pstTableHead==NULL)
    {
        return -10;
    }

    if((dwStartPos==0) || (dwStartPos>=g_stTable.pstTableHead->dwAllElementCount))
    {
        // Position gone insane
        Attr_API(161639 , 1);
        return -2;
    }

    dwNextPos=dwStartPos;
    while((dwNextPos) &&(iCount<=MAX_BLOCK_COUNT))
    {
        dwCurPos=dwNextPos;
        if(dwCurPos>=g_stTable.pstTableHead->dwAllElementCount)
        {
            // Position gone insane
            Attr_API(161639 , 1);
            return -3;
        }

        dwNextPos=GET_ELE_AT(dwCurPos)->dwNext;
        memset(GET_ELE_AT(dwCurPos),0,g_stTable.pstTableHead->dwElementSize);

        // 把节点放入Free列表中
        GET_ELE_AT(dwCurPos)->dwNext=g_stTable.pstTableHead->dwFirstFreePos;
        g_stTable.pstTableHead->dwFirstFreePos=dwCurPos;

        g_stTable.pstTableHead->dwFreeElementCount++;
        iCount++;
    }

    if(dwNextPos!=0)
    {
        // Too many blocks
        Attr_API(161640 , 1);
        return -4;
    }

    return 0;
}

inline int TranslateIndexId(uint32_t dwKey, uint32_t *pdwIndexId)
{
    if(g_stTable.pstTableHead==NULL || pdwIndexId==NULL)
    {
        return -10;
    }

    *pdwIndexId = (dwKey/g_stTable.pstTableHead->dwIndexRowCount + 1);
    return 0;
}

inline int TranslateIndexOffset(uint32_t dwKey, uint32_t *pdwIndexOffset)
{
    if(g_stTable.pstTableHead==NULL || pdwIndexOffset==NULL)
    {
        return -10;
    }

    *pdwIndexOffset = dwKey % g_stTable.pstTableHead->dwIndexRowCount;
    return 0;
}

inline int TranslateKey(uint32_t *pdwKey, uint32_t dwIndexId, uint32_t dwIndexOffset)
{
    if(g_stTable.pstTableHead==NULL || pdwKey==NULL)
    {
        return -10;
    }

    *pdwKey = (dwIndexId-1) * g_stTable.pstTableHead->dwIndexRowCount + dwIndexOffset;
    return 0;
}



static int GetIndexNode(uint32_t dwKey, IndexNode **ppIndexNode, int createFlag)
{
    uint32_t dwIndexId=0, dwIndexOffset=0;
    IndexHead *pih = NULL;

    if(g_stTable.pstTableHead==NULL)
    {
        return -10;
    }

    TranslateIndexId(dwKey, &dwIndexId);
    TranslateIndexOffset(dwKey, &dwIndexOffset);

    GetIndexHead(dwIndexId, &pih, createFlag);

    if(pih)
        *ppIndexNode = &(g_stTable.pstIndexNodeList[pih->iIndex*g_stTable.pstTableHead->dwIndexRowCount+dwIndexOffset]);
    else
        *ppIndexNode = NULL;

    return 0;
}

int GetSequence(uint32_t dwIndexId, uint64_t *pddwSequence)
{
    IndexHead *pih = NULL;

    if(pddwSequence==NULL)
    {
        return -1;
    }

    if(g_stTable.pstTableHead==NULL)
    {
        return -10;
    }

    GetIndexHead(dwIndexId, &pih, 0);

    if(pih==NULL)
    {
        return -20;
    }

    *pddwSequence = pih->ddwSequence;
    return 0;
}

int SetSequence(uint32_t dwIndexId, uint64_t ddwSequence)
{
    IndexHead *pih = NULL;

    if(g_stTable.pstTableHead==NULL)
    {
        return -10;
    }

    GetIndexHead(dwIndexId, &pih, 0);

    if(pih==NULL)
    {
        return -20;
    }

    pih->ddwSequence = ddwSequence;
    return 0;
}


//取得用户数据
/*
<0:错误
=0:找到数据
>0:不存在
*/
int GetCacheData(uint32_t dwKey, char *sDataBuf, int *piDataLen)
{
    IndexNode *pIndexNode=NULL;
    uint32_t  dwCurPos=0,dwNextPos=0;
    int iRet=0;
    int iDataLen=0,iBufLen=0;

    if(g_stTable.pstTableHead==NULL)
    {
        return -10;
    }

    if(sDataBuf==NULL || piDataLen==NULL)
    {
        return -1;
    }

    iBufLen=*piDataLen;
    *piDataLen=0;
    if(iBufLen < (int)ELEMENT_DATABUF_LEN)
    {
        return -2;
    }

    iRet=GetIndexNode(dwKey,&pIndexNode,0); // do not create
    if(iRet<0)
    {
        return -3;
    }

    //index不存在或者未初始化返回不存在
    if((pIndexNode==NULL)||(pIndexNode->cFlag==0))
    {
        return 1;
    }

    dwNextPos=pIndexNode->dwPosition;
    while(dwNextPos)
    {
        dwCurPos=dwNextPos;
        if(dwCurPos>=g_stTable.pstTableHead->dwAllElementCount)
        {
            Attr_API(161639, 1);
            return -21;
        }
        if((iDataLen+(int)ELEMENT_DATABUF_LEN) > iBufLen)
        {
            Attr_API(161640, 1);
            return -4;
        }
        if((uint32_t)GET_ELE_AT(dwCurPos)->dwKey!=dwKey)
        {
            Attr_API(161641, 1);
            return -5;
        }
        memcpy(sDataBuf+iDataLen,GET_ELE_AT(dwCurPos)->bufData,
        	ELEMENT_DATABUF_LEN);
        iDataLen+=ELEMENT_DATABUF_LEN;
        dwNextPos=GET_ELE_AT(dwCurPos)->dwNext;
    }
    *piDataLen=iDataLen;

    return 0;
}

int PrintCacheData(uint32_t dwKey)
{
    IndexNode *pIndexNode = NULL;
    uint32_t dwCurPos = 0, dwNextPos = 0;
    int iRet = 0;

    if(g_stTable.pstTableHead == NULL)
    {
        return -10;
    }

    iRet = GetIndexNode(dwKey, &pIndexNode, 0);
    if(iRet < 0)
    {
        return -20;
    }

    //index不存在或者未初始化返回不存在
    if((pIndexNode == NULL) || (pIndexNode->cFlag == 0))
    {
        if(pIndexNode == NULL)
            printf("No index node found.\n");
        else
            printf("cFlag is zero, dwPosition=%u.\n", pIndexNode->dwPosition);
        return 11;
    }

    dwNextPos = pIndexNode->dwPosition;
    printf("dwPosition=%u, dwKey=%u\n", dwNextPos, dwKey);
    while(dwNextPos)
    {
        dwCurPos = dwNextPos;
        printf("dwCurPos=%u, dwKey=%u", dwCurPos, (uint32_t)GET_ELE_AT(dwCurPos)->dwKey);
        printf("\n%s", DumpMemory(GET_ELE_AT(dwCurPos)->bufData, 0, ELEMENT_DATABUF_LEN));
        dwNextPos = GET_ELE_AT(dwCurPos)->dwNext;
    }

    return 0;
}

int CheckIfOldUnit(uint32_t dwUnitId)
{
	int ret;
	IndexHead *pih;

	ret = GetIndexHead(dwUnitId+1, &pih, 0);

	return ret==0? 1 : 0;
}

//设置数据,如果存在删除已有数据
int SetCacheData(uint32_t dwKey, char *sDataBuf, int iDataLen)
{
    IndexNode *pIndexNode=NULL;
    uint32_t dwCurPos=0,dwNextPos=0,dwStartPos=0,dwOldPos=0;
    int iCount=0,iLeftLen=0,iCopyLen=0;
    int iRet=0;

    if(g_stTable.pstTableHead==NULL)
    {
        return -10;
    }

    if(sDataBuf==NULL || iDataLen<0)
    {
        return -1;
    }

    iCount=(iDataLen+ELEMENT_DATABUF_LEN-1)/ELEMENT_DATABUF_LEN;
    if(iCount>MAX_BLOCK_COUNT)
    {
        return -2;
    }

    //创建或取得 IndexNode
    iRet=GetIndexNode(dwKey, &pIndexNode, 1); // create if not found
    if(iRet<0)
    {
        return -3;
    }

    if(pIndexNode==NULL) //IndexHeadList空间不够，需要扩容
    {
        // 出现这种情况，很有可能是传入了非法的 UIN 所致
        return -4;
    }

    //先构造新数据
    iRet=GetEmptyElement(iCount, &dwStartPos);
    if(iRet<0)
    {
        return -7;
    }

    dwNextPos=dwStartPos;
    iLeftLen=iDataLen-iCopyLen;
    while((dwNextPos) && (iLeftLen>0))
    {
        dwCurPos=dwNextPos;
        if(dwCurPos>=g_stTable.pstTableHead->dwAllElementCount)
        {
            Attr_API(161639 , 1);
            return -8;
        }

        if(iLeftLen > (int)ELEMENT_DATABUF_LEN)
        {
            memcpy(GET_ELE_AT(dwCurPos)->bufData,
                sDataBuf+iCopyLen,ELEMENT_DATABUF_LEN);
            iCopyLen+=ELEMENT_DATABUF_LEN;
        }
        else
        {
            memcpy(GET_ELE_AT(dwCurPos)->bufData,
                sDataBuf+iCopyLen,(unsigned)iLeftLen);
            iCopyLen+=iLeftLen;
        }

        dwNextPos=GET_ELE_AT(dwCurPos)->dwNext;
        GET_ELE_AT(dwCurPos)->dwKey=dwKey;
        g_stTable.pstTableHead->dwFreeElementCount--;
        iLeftLen=iDataLen-iCopyLen;
    }

    if(iLeftLen!=0)
    {
        //bug
        return -9;
    }

    GET_ELE_AT(dwCurPos)->dwNext=0;
    GET_ELE_AT(0)->dwNext=dwNextPos;
    dwOldPos=pIndexNode->dwPosition;
    pIndexNode->dwPosition=dwStartPos;
    pIndexNode->cFlag=1;

    //删除旧数据
    if(dwOldPos!=0)
    {
        iRet=PreRecycleElement(dwOldPos);
        if(iRet<0)
        {
            return -6;
        }
    }

    return 0;
}


//删除指定索引下的所有数据包括索引定义(*删除整个Unit的接口*)
int ClearIndexData(uint32_t dwIndexId)
{
    int i=0,iRet=0, iIndex;
    uint32_t dwIndexStart=0, sz;
    IndexHead *pstIndexHead=NULL;
    IndexNode *pIndexNode=NULL;

    if((g_stTable.pstTableHead==NULL) || (dwIndexId==0))
    {
        return -10;
    }

    // If IndexIds are loaded at init time, no further modification is allowed
    if(g_stTable.pstTableHead->bLockedIndexIds)
    {
        return -9;
    }

    iRet = GetIndexHead(dwIndexId, &pstIndexHead, 0);
    if(iRet!=0 || pstIndexHead==NULL) // 找不到
    {
        return -1;
    }

    dwIndexStart = pstIndexHead->iIndex*g_stTable.pstTableHead->dwIndexRowCount;

    for(i=0;i<(int)g_stTable.pstTableHead->dwIndexRowCount;i++)
    {
        pIndexNode=&(g_stTable.pstIndexNodeList[dwIndexStart+i]);
        if(pIndexNode->dwPosition!=0)
        {
            iRet=RecycleElement(pIndexNode->dwPosition);
            if(iRet<0)
            {
                Attr_API(241914, 1);
                return -3;
            }
            pIndexNode->dwPosition=0;
            pIndexNode->cFlag=0;
        }
    }

    sz = (unsigned long)(g_stTable.pstIndexHeadList+g_stTable.pstTableHead->dwUsedIndexCount) -
            (unsigned long)(pstIndexHead+1);
    iIndex = pstIndexHead->iIndex;
    if(sz>0) // move one element backwards
    memmove(pstIndexHead, pstIndexHead+1, sz);
    g_stTable.pstIndexHeadList[g_stTable.pstTableHead->dwUsedIndexCount-1].iIndex = iIndex;
    g_stTable.pstIndexHeadList[g_stTable.pstTableHead->dwUsedIndexCount-1].dwIndexId = 0;
    g_stTable.pstTableHead->dwUsedIndexCount --;

    return 0;
}

// 判断初始化的时候是不是调用了SetLinkTableUnits()来设置UnitId
// 如果是，则不允许增加或删除UnitId， 函数返回 1，否则返回0
int IsIndexIdsLocked()
{
    if((g_stTable.pstTableHead==NULL))
    {
        return 0;
    }
    return g_stTable.pstTableHead->bLockedIndexIds? 1:0;
}


//删除指定索引下的所有数据包括索引定义
int ClearIndexHeadAndNode(uint32_t dwIndexId)
{
    int i=0, iRet=0;
    uint32_t dwIndexStart=0, sz;
    int iIndex;
    IndexHead *pstIndexHead=NULL;
    IndexNode *pIndexNode=NULL;

    if((g_stTable.pstTableHead==NULL) ||(dwIndexId==0))
    {
        return -10;
    }

    // If IndexIds are loaded at init time, no further modification is allowed
    if(g_stTable.pstTableHead->bLockedIndexIds)
    {
        return -9;
    }

    iRet = GetIndexHead(dwIndexId, &pstIndexHead, 0);
    if(iRet!=0 || pstIndexHead==NULL) // 找不到
    {
        return -1;
    }

    dwIndexStart = pstIndexHead->iIndex*g_stTable.pstTableHead->dwIndexRowCount;

	for(i=0;i<(int)g_stTable.pstTableHead->dwIndexRowCount;i++)
    {
        pIndexNode=&(g_stTable.pstIndexNodeList[dwIndexStart+i]);
        if(pIndexNode->dwPosition!=0)	// 还有没有释放完毕的，返回错误
        {
            return 10;
        }
    }

    sz = (unsigned long)(g_stTable.pstIndexHeadList+g_stTable.pstTableHead->dwUsedIndexCount) -
            (unsigned long)(pstIndexHead+1);
    iIndex = pstIndexHead->iIndex;
    if(sz>0)
        memmove(pstIndexHead, pstIndexHead+1, sz);
    g_stTable.pstIndexHeadList[g_stTable.pstTableHead->dwUsedIndexCount-1].iIndex = iIndex;
    g_stTable.pstIndexHeadList[g_stTable.pstTableHead->dwUsedIndexCount-1].dwIndexId = 0;
    g_stTable.pstTableHead->dwUsedIndexCount --;

    return 0;
}

static int ValifyIndex(void)
{
    int i, j, dup=0;

    for(i=0; i<(int)g_stTable.pstTableHead->dwIndexCount; i++)
    {
        for(j=0; j<i; j++)
        {
            if(g_stTable.pstIndexHeadList[j].iIndex==g_stTable.pstIndexHeadList[i].iIndex)
                break;
        }

        if(j<i) // found duplicate
        {
            printf("Warning: duplicate index found between %d and %d, value=%d.\n", i, j, g_stTable.pstIndexHeadList[i].iIndex);
            dup ++;
        }
    }

    return dup;
}

#define PRINT_ALL_ELEMENTS 0

int PrintLinkTableInfo(void)
{
    int i=0;
    unsigned long total=0, unit_total;

    if(g_stTable.pstTableHead==NULL)
    {
        return -10;
    }

    printf("dwAllElementCount:  %u\n",g_stTable.pstTableHead->dwAllElementCount);
    printf("dwFreeElementCount: %u\n",g_stTable.pstTableHead->dwFreeElementCount);
    printf("dwElementSize:      %u\n",g_stTable.pstTableHead->dwElementSize);
    printf("dwIndexCount:       %u\n",g_stTable.pstTableHead->dwIndexCount);
    printf("dwIndexRowCount:    %u\n",g_stTable.pstTableHead->dwIndexRowCount);
    printf("dwUsedIndexCount:   %u\n",g_stTable.pstTableHead->dwUsedIndexCount);
    printf("dwFirstFreePos:     %u\n",g_stTable.pstTableHead->dwFirstFreePos);
    printf("dwPreFreeIndex:     %u\n",g_stTable.pstTableHead->dwPreFreeIndex);

    for(total=0,i=0; i<LT_PRE_FREE_POOL_SIZE; i++)
    {
        if(g_stTable.pstTableHead->adwPreFreePool[i])
            total ++;
    }
    printf("dwPreFreeCount:     %lu\n",total);

    total=0;

#if 0
    for(i=0; i<(int)g_stTable.pstTableHead->dwIndexCount; i++)
    {
        printf("%d: iIndex:%d, dwIndexId:%u\n", i,
                g_stTable.pstIndexHeadList[i].iIndex,
                g_stTable.pstIndexHeadList[i].dwIndexId);
    }
#else
    ValifyIndex();
#endif

    for(i=0; i<(int)g_stTable.pstTableHead->dwIndexCount; i++)
    {
        if(g_stTable.pstIndexHeadList[i].dwIndexId != 0)
        {
            unsigned int j;
            uint32_t start_idx=g_stTable.pstIndexHeadList[i].iIndex*g_stTable.pstTableHead->dwIndexRowCount;

#if PRINT_ALL_ELEMENTS
            uint32_t start_uin=(g_stTable.pstIndexHeadList[i].dwIndexId-1)*g_stTable.pstTableHead->dwIndexRowCount;

            printf("dwIndexId:%u (UnitID:%u) iIndex: %d\n",
                    g_stTable.pstIndexHeadList[i].dwIndexId, g_stTable.pstIndexHeadList[i].dwIndexId-1,
                    g_stTable.pstIndexHeadList[i].iIndex);
#endif
            unit_total = 0;
            for(j=0; j<g_stTable.pstTableHead->dwIndexRowCount; j++)
            {
                if(g_stTable.pstIndexNodeList[start_idx+j].cFlag!=0)
                {
                    uint32_t pos = g_stTable.pstIndexNodeList[start_idx+j].dwPosition;
                    if(pos>g_stTable.pstTableHead->dwAllElementCount)
                        printf("\tpstIndexNodeList[%u].dwPosition is invalid!\n", start_idx+j);
#if PRINT_ALL_ELEMENTS
                    else
                        printf("\tpstElementList[%d]: key=%llu, next=%u, uin=%lu\n",
                            pos, (uint32_t)GET_ELE_AT(pos)->dwKey,
                            GET_ELE_AT(pos)->dwNext, start_uin+(uint32_t)j);
#endif
                    total++;
                    unit_total++;
                }
            }
#if PRINT_ALL_ELEMENTS
            printf("UINs in unit: %lu\n", unit_total);
#else
            printf("dwIndexId:%u (UnitID:%u), iIndex: %d, uins:%lu\n",
                    g_stTable.pstIndexHeadList[i].dwIndexId, g_stTable.pstIndexHeadList[i].dwIndexId-1,
                    g_stTable.pstIndexHeadList[i].iIndex, unit_total);
#endif
        }
    }

    printf("\nTotal UINs: %lu\n", total);
    return 0;
}

int PrintLinkTableElements(void)
{
    int i=0;
    unsigned long total=0;

    if(g_stTable.pstTableHead==NULL)
    {
        return -10;
    }

    ValifyIndex();

    printf("dwAllElementCount:  %u\n",g_stTable.pstTableHead->dwAllElementCount);
    printf("dwFreeElementCount: %u\n",g_stTable.pstTableHead->dwFreeElementCount);
    printf("dwElementSize:      %u\n",g_stTable.pstTableHead->dwElementSize);
    printf("dwIndexCount:       %u\n",g_stTable.pstTableHead->dwIndexCount);
    printf("dwIndexRowCount:    %u\n",g_stTable.pstTableHead->dwIndexRowCount);
    printf("dwUsedIndexCount:   %u\n",g_stTable.pstTableHead->dwUsedIndexCount);
    printf("dwFirstFreePos:     %u\n",g_stTable.pstTableHead->dwFirstFreePos);
    printf("dwPreFreeIndex:     %u\n",g_stTable.pstTableHead->dwPreFreeIndex);

    for(total=0,i=0; i<LT_PRE_FREE_POOL_SIZE; i++)
    {
        if(g_stTable.pstTableHead->adwPreFreePool[i])
            total ++;
    }
    printf("dwPreFreeCount:     %lu\n",total);

    total=0;

    printf("\nElements:\n");
    for(i=0; i<=(int)g_stTable.pstTableHead->dwAllElementCount; i++)
    {
        if(GET_ELE_AT(i)->dwNext || GET_ELE_AT(i)->dwKey)
        {
            printf("\tpstElementList[%d]: next=%u, key=%u\n",i,
                    GET_ELE_AT(i)->dwNext, (uint32_t)GET_ELE_AT(i)->dwKey);

            if(GET_ELE_AT(i)->dwKey)
                total ++;
        }
    }

    printf("\nTotal elements: %lu\n", total);
    return 0;
}

//删除指定索引下的所有数据包括索引定义
int ClearIndexNodeData(uint32_t dwKey)
{
    IndexNode *pIndexNode=NULL;
    int iRet=0;

    if(g_stTable.pstTableHead==NULL)
    {
        return -10;
    }

    iRet=GetIndexNode(dwKey, &pIndexNode, 0);
    if(iRet<0)
    {
        return -20;
    }

    //index不存在或者未初始化返回不存在
    if(pIndexNode==NULL || pIndexNode->cFlag==0)
    {
        return 10;
    }

    if(pIndexNode->dwPosition!=0)
    {
        uint32_t dwOldPos = pIndexNode->dwPosition;

        // 注意这个顺序，程序有可能随时被killed，优先保证指针有效性
        pIndexNode->cFlag=0;
        pIndexNode->dwPosition=0;

        iRet=PreRecycleElement(dwOldPos);
        if(iRet<0)
        {
            return -30;
        }
    }

    return 0;
}


int CloseLinkTable()
{
    if(g_stTable.pstTableHead!=NULL)
        shmdt(g_stTable.pstTableHead);

    memset((void*)&g_stTable, 0, sizeof(g_stTable));
    return 0;
}

volatile LinkTable *GetLinkTable()
{
    if(g_stTable.pstTableHead==NULL)
    {
        return NULL;
    }
    return &g_stTable;
}

