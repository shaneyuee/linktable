/******************************************************************************

 FILENAME:	LinkTable.c

  DESCRIPTION:		��������+����ʽ�ڴ�ӿڶ���

 HISTORY:
           Date          Author         Comment
        2008-05-15      RunnerWang      Created
        2010-09-04      Forestli        ֧��64λuin
        2011-09-14      Shaneyu         ��IndexHead��������Ͷ��ֲ���
        2011-11-30      Shaneyu         �����нڵ�����Free��������32λkey
        2012-05-01      Shaneyu         ���̶���С������ĳɿ����ô�С�Ŀ�
        2012-09-13      Shaneyu         ����Ԥ���ճأ���ֹ��д��ͻ
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
        return -2; //����ķ����룬ע�Ᵽ��
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

// ������дһ�£�֧��unsigned size������ӡ������mlock
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

    // ��ʼ������ֵΪ�������ڵ�����
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

// ��֤SHM�еĲ��������õĲ����Ƿ�һ�£���һ���򷵻�ʧ��
// ����ֵ�� 0 ������  1 �����Ҳ����Ϸ�  -1 ��������
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

    // ��������ʱ��Ԥ���ճ��е�����Ԫ�ػ��յ�
    if(iCreateFlag==1)
        RecycleAllPreFreeElements();
    return 0;
}

// ���pstIndexHeadListΪ�գ���LinkTable��pstIndexHeadList��ʼ��Ϊ��Ӧ��UnitList
// ���򣬱Ƚ�pstIndexHeadList��dwUnitList����������Ӧ�򷵻�ʧ��
// dwUnitList�������Ѿ��ź����
// �ɹ�����0�����򷵻ظ���
int SetLinkTableUnits(uint32_t dwUnitList[], int iUnitNum)
{
    int i;

    if(g_stTable.pstTableHead==NULL || dwUnitList==NULL ||
         iUnitNum<0 || iUnitNum>(int)g_stTable.pstTableHead->dwIndexCount)
        return -1;

    // ��� dwUnitList�Ƿ��Ѿ��ź���
    for(i=0; i<iUnitNum-1; i++)
    {
        if(dwUnitList[i]>=dwUnitList[i+1])
            return -2;
    }

    if(g_stTable.pstTableHead->dwUsedIndexCount>0) // IndexHeadList��Ϊ��
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
    else // IndexHeadListΪ��
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
        // createFlag=1: û�пռ��ˣ���Ҫ��������
        // createFlag=0: �Ҳ���
    {
        return -3;
    }
    else if(ret==0) // �ҵ����ڵ���
    {
    }
    else // �Ҳ����������µ�
    {
        g_stTable.pstTableHead->dwUsedIndexCount ++;
    }
    return ret;
}

static int RecycleElement(uint32_t dwStartPos);

// ����ȫ��Ԥ���ճ��е�Ԫ��
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
                // �������ʧ�ܣ������µ�Ԫ���ܱ��������ã�����ֻ�ϱ���������
                Attr_API(241914, 1);
            }

            g_stTable.pstTableHead->adwPreFreePool[i] = 0;
        }
    }

    g_stTable.pstTableHead->dwPreFreeIndex = 0;
}

// Ԥ���ճص���Ҫ�㷨ʵ�֣�
// ��һ���α�dwPreFreeIndexѭ������Ԥ���ճ�ָ������adwPreFreePool��û��Ԥ���ձ���һ��ָ�룬
// ������ϣ�����յ���ʼ��ַ�ҵ�adwPreFreePool[dwPreFreeIndex]�ϣ��������ϴιҵ���ĵط���Ԫ������
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

    // �ȷŵ�Ԥ���ճ��У��ٻ���ͬ��λ���еľ�����
    dwOldPos = g_stTable.pstTableHead->adwPreFreePool[dwIdx];
    g_stTable.pstTableHead->adwPreFreePool[dwIdx] = dwStartPos;
    g_stTable.pstTableHead->dwPreFreeIndex = (dwIdx+1)%LT_PRE_FREE_POOL_SIZE;

    if(dwOldPos)
    {
        iRet = RecycleElement(dwOldPos);
        if(iRet<0)
        {
            // �������ʧ�ܣ������µ�Ԫ���ܱ��������ã�����ֻ�ϱ���������
            Attr_API(241914, 1);
        }
    }

    return 0;
}


//ʹ�����鷽ʽ�������ɿռ䣬�����ʹ�����ɿռ���ò�����ʽ
static int GetEmptyElement(int iCount, uint32_t *pdwStartPos)
{
    uint32_t  dwStartPos,dwCurPos;
    int i,j,iTrySecond=0;
    float fUsedRate;

    if(g_stTable.pstTableHead==NULL)
    {
        return -1;
    }

    // ֻҪ���п���С��Ԥ������͸澯
    fUsedRate = (g_stTable.pstTableHead->dwFreeElementCount*100.0/g_stTable.pstTableHead->dwAllElementCount);
    if(fUsedRate<20.0) Attr_API(178992, 1);
    if(fUsedRate<15.0) Attr_API(232399, 1);
    if(fUsedRate<10.0) Attr_API(241997, 1);
    if(fUsedRate<5.0)  Attr_API(242000, 1);

restart:
    if(iTrySecond) // ���Ԥ���ճغ����·���
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

    // �ȴ�Free�б��������Ѳ����ٰ�������
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
        if(i==iCount) // �����һ����Next����
            GET_ELE_AT(dwCurPos)->dwNext = 0;
        dwCurPos=dwNext;
    }

    if(i==iCount) // �ҵ���
    {
        *pdwStartPos = g_stTable.pstTableHead->dwFirstFreePos;
        g_stTable.pstTableHead->dwFirstFreePos = dwCurPos;
        Attr_API(178921, 1);
        if(iTrySecond)
            Attr_API(242024, 1);
        return 0;
    }

    // FIXME���������һ�����Ǻ���Ҫ�����⣺���ǵ���Free�������ҵ�һЩ�ڵ㣬���޷������������Ҫ��
    // ��ʱ��Ҫͨ���������������䣬�������������ܻ��ҵ�free�����еĽڵ㣬�⽫����free�����еĽڵ㱻
    // ���������ˣ�ʹ��Free���������������������ֻ���ɱ�����������������

    // ��pstTableHead->dwLastEmpty��ʼ���а�������
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

        // �ѽڵ����Free�б���
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


//ȡ���û�����
/*
<0:����
=0:�ҵ�����
>0:������
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

    //index�����ڻ���δ��ʼ�����ز�����
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

    //index�����ڻ���δ��ʼ�����ز�����
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

//��������,�������ɾ����������
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

    //������ȡ�� IndexNode
    iRet=GetIndexNode(dwKey, &pIndexNode, 1); // create if not found
    if(iRet<0)
    {
        return -3;
    }

    if(pIndexNode==NULL) //IndexHeadList�ռ䲻������Ҫ����
    {
        // ����������������п����Ǵ����˷Ƿ��� UIN ����
        return -4;
    }

    //�ȹ���������
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

    //ɾ��������
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


//ɾ��ָ�������µ��������ݰ�����������(*ɾ������Unit�Ľӿ�*)
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
    if(iRet!=0 || pstIndexHead==NULL) // �Ҳ���
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

// �жϳ�ʼ����ʱ���ǲ��ǵ�����SetLinkTableUnits()������UnitId
// ����ǣ����������ӻ�ɾ��UnitId�� �������� 1�����򷵻�0
int IsIndexIdsLocked()
{
    if((g_stTable.pstTableHead==NULL))
    {
        return 0;
    }
    return g_stTable.pstTableHead->bLockedIndexIds? 1:0;
}


//ɾ��ָ�������µ��������ݰ�����������
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
    if(iRet!=0 || pstIndexHead==NULL) // �Ҳ���
    {
        return -1;
    }

    dwIndexStart = pstIndexHead->iIndex*g_stTable.pstTableHead->dwIndexRowCount;

	for(i=0;i<(int)g_stTable.pstTableHead->dwIndexRowCount;i++)
    {
        pIndexNode=&(g_stTable.pstIndexNodeList[dwIndexStart+i]);
        if(pIndexNode->dwPosition!=0)	// ����û���ͷ���ϵģ����ش���
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

//ɾ��ָ�������µ��������ݰ�����������
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

    //index�����ڻ���δ��ʼ�����ز�����
    if(pIndexNode==NULL || pIndexNode->cFlag==0)
    {
        return 10;
    }

    if(pIndexNode->dwPosition!=0)
    {
        uint32_t dwOldPos = pIndexNode->dwPosition;

        // ע�����˳�򣬳����п�����ʱ��killed�����ȱ�ָ֤����Ч��
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

