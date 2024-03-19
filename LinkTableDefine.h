/******************************************************************************

 FILENAME:	LinkTableDefine.h

  DESCRIPTION:	��������+����ʽ�ڴ�ṹ����

 HISTORY:
           Date          Author         Comment
        2008-05-15      RunnerWang      Created
        2010-09-04      Forestli        ֧��64λuin
        2011-09-14      Shaneyu         ��IndexHead��������Ͷ��ֲ���
        2011-11-30      Shaneyu         �����нڵ�����Free��������32λkey
        2012-05-01      Shaneyu         ���̶���С������ĳɿ����ô�С�Ŀ�
        2012-09-13      Shaneyu         ����Ԥ���ճأ���ֹ��д��ͻ
******************************************************************************/

#ifndef _LINK_TABLE_DEFINE_H_
#define _LINK_TABLE_DEFINE_H_

#define MAX_SHM_SIZE (1024U*1024*640*4)
#define MAX_BLOCK_COUNT (200)
#define MAX_ELEMENT_SIZE (200)

typedef struct
{
    uint32_t dwPosition:31;
    uint32_t cFlag:1;
} IndexNode;

typedef struct
{
    uint32_t dwIndexId;
    uint64_t ddwSequence;
    int iIndex;
}IndexHead;

typedef struct
{
    uint32_t dwKey;
    uint32_t dwNext;
    uint8_t bufData[0];
}Element;

#define _GET_ELE_AT(elist, idx, sz) ((Element *)(((char *)(elist)) + ((idx) * (sz))))

#if 0
#define GET_ELE_AT(idx) ({\
    volatile LinkTable *_p_l_t;\
    _p_l_t=GetLinkTable();\
    _p_l_t? _GET_ELE_AT(_p_l_t->pstElementList, idx, _p_l_t->pstTableHead->dwElementSize):((Element *)0);\
})
#endif

// �ⲿʹ��
#define LT_ELE_AT(pLT, idx) _GET_ELE_AT((pLT)->pstElementList, idx, (pLT)->pstTableHead->dwElementSize)

// �ڲ�ʹ��
#define GET_ELE_AT(idx) LT_ELE_AT(&g_stTable, idx)

#define LT_PRE_FREE_POOL_SIZE   100
// ��֤����LT_PRE_FREE_POOL_SIZE�����ڴ��С����
#define LT_MAX_RESERVE_LEN      (1024*1024+400-(LT_PRE_FREE_POOL_SIZE*4))


typedef struct
{
    uint32_t dwIndexCount;
    uint32_t dwIndexRowCount;
    uint32_t dwAllElementCount;
    uint32_t dwFreeElementCount;
    uint32_t dwLastEmpty;
    uint32_t dwUsedIndexCount;
    uint8_t bLockedIndexIds;
    uint8_t sRerve[3];
    uint32_t dwElementSize;
    uint32_t dwFirstFreePos; // All free elements are linked together by dwNext
    uint32_t dwPreFreeIndex; // Current index to adwPreFreePool
    uint32_t adwPreFreePool[LT_PRE_FREE_POOL_SIZE]; // All elements to be deleted are put here for delayed deletion
    uint8_t  sReserveInfo[LT_MAX_RESERVE_LEN];  // Reserved for application use
}LinkTableHead;


typedef struct
{
    LinkTableHead *pstTableHead;
    IndexHead *pstIndexHeadList;
    IndexNode *pstIndexNodeList;
    Element  *pstElementList;
}LinkTable;

volatile LinkTable *GetLinkTable();

#endif
