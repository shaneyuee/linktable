/******************************************************************************

 FILENAME:	LinkTable.h

  DESCRIPTION:	数组索引+链表方式内存结构接口声明

 HISTORY:
           Date          Author         Comment
        2008-05-15      RunnerWang      Created
        2010-09-04      Forestli        支持64位uin
        2011-09-14      Shaneyu         对IndexHead进行排序和二分查找
        2011-11-30      Shaneyu         将空闲节点存放在Free链表，滚回32位key
        2012-05-01      Shaneyu         将固定大小的数组改成可配置大小的块
        2012-09-13      Shaneyu         增加预回收池，防止读写冲突
 ******************************************************************************/

#ifndef _LINK_TABLE_H_
#define _LINK_TABLE_H_

//初始化链式表
int InitLinkTable(uint32_t dwIndexCount, uint32_t dwIndexRowCount,
    uint32_t dwElementCount, uint32_t dwElementSize, int iKey, int iCreateFlag);

// 如果pstIndexHeadList为空，则将LinkTable的pstIndexHeadList初始化为相应的UnitList
// 否则，比较pstIndexHeadList和dwUnitList，如果不相对应则返回失败
// dwUnitList必须是已经排好序的
// 成功返回0，否则返回负数
int SetLinkTableUnits(uint32_t dwUnitList[], int iUnitNum);

// 判断初始化的时候是不是调用了SetLinkTableUnits()来设置UnitId
// 如果是，则不允许增加或删除UnitId， 函数返回 1，否则返回0
int IsIndexIdsLocked();

//取得用户数据
int GetCacheData(uint32_t dwKey, char *sDataBuf, int *piDataLen);

int GetSequence(uint32_t dwIndexId, uint64_t *pddwSequence);
int SetSequence(uint32_t dwIndexId, uint64_t ddwSequence);

//打印用户数据的链表指针
int PrintCacheData(uint32_t dwKey);

//设置数据,如果存在删除已有数据
int SetCacheData(uint32_t dwKey, char *sDataBuf, int iDataLen);

//删除指定索引下的所有数据包括索引定义
int ClearIndexData(uint32_t dwIndexId);

//清除某个Key对应的数据
int ClearIndexNodeData(uint32_t dwKey);

//释放头部
int ClearIndexHeadAndNode(uint32_t dwIndexId);
int PrintLinkTableInfo(void);
int PrintLinkTableElements(void);

//关闭链式表共享内存
int CloseLinkTable();

int CheckIfOldUnit(uint32_t dwUnitId);

#endif
