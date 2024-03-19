/******************************************************************************

 FILENAME:	LinkTable.h

  DESCRIPTION:	��������+����ʽ�ڴ�ṹ�ӿ�����

 HISTORY:
           Date          Author         Comment
        2008-05-15      RunnerWang      Created
        2010-09-04      Forestli        ֧��64λuin
        2011-09-14      Shaneyu         ��IndexHead��������Ͷ��ֲ���
        2011-11-30      Shaneyu         �����нڵ�����Free��������32λkey
        2012-05-01      Shaneyu         ���̶���С������ĳɿ����ô�С�Ŀ�
        2012-09-13      Shaneyu         ����Ԥ���ճأ���ֹ��д��ͻ
 ******************************************************************************/

#ifndef _LINK_TABLE_H_
#define _LINK_TABLE_H_

//��ʼ����ʽ��
int InitLinkTable(uint32_t dwIndexCount, uint32_t dwIndexRowCount,
    uint32_t dwElementCount, uint32_t dwElementSize, int iKey, int iCreateFlag);

// ���pstIndexHeadListΪ�գ���LinkTable��pstIndexHeadList��ʼ��Ϊ��Ӧ��UnitList
// ���򣬱Ƚ�pstIndexHeadList��dwUnitList����������Ӧ�򷵻�ʧ��
// dwUnitList�������Ѿ��ź����
// �ɹ�����0�����򷵻ظ���
int SetLinkTableUnits(uint32_t dwUnitList[], int iUnitNum);

// �жϳ�ʼ����ʱ���ǲ��ǵ�����SetLinkTableUnits()������UnitId
// ����ǣ����������ӻ�ɾ��UnitId�� �������� 1�����򷵻�0
int IsIndexIdsLocked();

//ȡ���û�����
int GetCacheData(uint32_t dwKey, char *sDataBuf, int *piDataLen);

int GetSequence(uint32_t dwIndexId, uint64_t *pddwSequence);
int SetSequence(uint32_t dwIndexId, uint64_t ddwSequence);

//��ӡ�û����ݵ�����ָ��
int PrintCacheData(uint32_t dwKey);

//��������,�������ɾ����������
int SetCacheData(uint32_t dwKey, char *sDataBuf, int iDataLen);

//ɾ��ָ�������µ��������ݰ�����������
int ClearIndexData(uint32_t dwIndexId);

//���ĳ��Key��Ӧ������
int ClearIndexNodeData(uint32_t dwKey);

//�ͷ�ͷ��
int ClearIndexHeadAndNode(uint32_t dwIndexId);
int PrintLinkTableInfo(void);
int PrintLinkTableElements(void);

//�ر���ʽ�����ڴ�
int CloseLinkTable();

int CheckIfOldUnit(uint32_t dwUnitId);

#endif
