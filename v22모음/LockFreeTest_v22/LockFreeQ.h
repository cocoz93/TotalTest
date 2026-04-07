#pragma once


#ifndef ____LOCKFREE_QUEUE_H____
#define ____LOCKFREE_QUEUE_H____

#include "LockFree_FreeList.h"

template<typename T>
class CLockFreeQ
{
	//-----------------------------------------------------
	struct NODE
	{
		T Data;
		NODE* pNextNode;
	};

	struct TopNODE
	{
		NODE* pNode;
		INT64 UniqueCount;
	};
	//-----------------------------------------------------



public:
	//최초 더미생성
	explicit CLockFreeQ(bool placementNew = false)
	{
		_pFreeList = new CLockFree_FreeList<NODE>(placementNew);

		_pDummy = _pFreeList->Alloc();
		_pDummy->pNextNode = nullptr;

		_phead = (TopNODE*)_aligned_malloc(sizeof(TopNODE), 16);
		_ptail = (TopNODE*)_aligned_malloc(sizeof(TopNODE), 16);

		_phead->pNode = (NODE*)this->_pDummy;
		_phead->UniqueCount = 0;

		_ptail->pNode = (NODE*)this->_pDummy;
		_ptail->UniqueCount = 0;

		_UseSize = 0;

		_HeadUniqueCount = 0;
		_TailUniqueCount = 0;
	}

	virtual ~CLockFreeQ()
	{
		Clear();

		_pFreeList->Free(this->_phead->pNode);

		_aligned_free((void*)this->_ptail);
		_aligned_free((void*)this->_phead);

		delete _pFreeList;
	}

	void Clear(void)
	{
		//모든 노드 삭제
		NODE* pfNode = nullptr;

		while (this->_phead->pNode->pNextNode != nullptr)
		{
			pfNode = this->_phead->pNode->pNextNode;
			this->_phead->pNode->pNextNode = this->_phead->pNode->pNextNode->pNextNode;
			_pFreeList->Free(pfNode);
		}

		_phead->UniqueCount = 0;
		_ptail->UniqueCount = 0;
		_ptail->pNode = _phead->pNode;

		_UseSize = 0;
		_HeadUniqueCount = 0;
		_TailUniqueCount = 0;
	}

	bool IsEmpty(void)
	{
		return (_UseSize == 0) && (this->_phead == nullptr);
	}


	bool Enqueue(T Data)
	{
		TopNODE bTopTailNode;						// backup TailTopNode;
		NODE* pbTailNextNode;						// backupTailNext Node;
		NODE* pnNode = this->_pFreeList->Alloc();	// NewNode;

		pnNode->Data = Data;
		pnNode->pNextNode = nullptr;						// Enqueue는 pNext가 nullptr일 경우에만 

		volatile INT64 lTailUniqueCount = InterlockedIncrement64(&this->_TailUniqueCount);

		// 노드가 추가되면 Enqueue성공 간주. tail밀기 실패는 상관X
		while (true)
		{
			// tail백업
			bTopTailNode.UniqueCount = this->_ptail->UniqueCount;
			bTopTailNode.pNode = this->_ptail->pNode;

			//tail의 Next백업
			pbTailNextNode = bTopTailNode.pNode->pNextNode;

			//_______________________________________________________________________________________
			// 
			//	tail뒤에 노드가 존재하는 경우 - 밀어준다.
			//_______________________________________________________________________________________
			if (nullptr != pbTailNextNode)
			{
				lTailUniqueCount = InterlockedIncrement64(&this->_TailUniqueCount);

				if (true == InterlockedCompareExchange128
				(
					(volatile INT64*)_ptail,
					(INT64)lTailUniqueCount,
					(INT64)bTopTailNode.pNode->pNextNode,
					(INT64*)&bTopTailNode
				))
				{
					//InterlockedIncrement64((LONG64*)&this->_UseSize);
				}
				continue;
			}
			//_______________________________________________________________________________________

			//_______________________________________________________________________________________
			// 
			//	Enqueue시도 (CAS)
			//_______________________________________________________________________________________
			else
			{
				if (nullptr == InterlockedCompareExchangePointer
				(
					(volatile PVOID*)&bTopTailNode.pNode->pNextNode,
					(PVOID)pnNode,
					(PVOID)pbTailNextNode
				))
				{
					// Enqueue 성공 
					// tail 밀어준다 (성공여부 판단x)
					if (true == InterlockedCompareExchange128
					(
						(volatile INT64*)_ptail,
						(INT64)lTailUniqueCount,
						(INT64)bTopTailNode.pNode->pNextNode,
						(INT64*)&bTopTailNode
					))
					{
						//InterlockedIncrement64((LONG64*)&this->_UseSize);
					}
					break;
				}
			}
			//_______________________________________________________________________________________
		}

		InterlockedIncrement64(&this->_UseSize);
		return true;
	}


	bool Dequeue(T* pOutData)
	{
		volatile INT64 lUseSize = InterlockedDecrement64(&_UseSize);

		if (lUseSize < 0)
		{
			volatile INT64 lCurSize = InterlockedIncrement64(&_UseSize);

			if (lCurSize <= 0)
			{
				pOutData = nullptr;
				return false;
			}
		}

		volatile INT64 lHeadUniqueCount = InterlockedIncrement64(&this->_HeadUniqueCount);
		volatile INT64 lTailUniqueCount;

		TopNODE	 bTopHeadNode;
		TopNODE	 bTopTailNode;
		NODE* pbTailNextNode;
		NODE* bHeadNextNode;

		while (true)
		{
			//_______________________________________________________________________________________
			// 
			//	tail뒤에 노드가 존재하는 경우 - 밀어준다.
			//_______________________________________________________________________________________

			// tail백업
			bTopTailNode.UniqueCount = this->_ptail->UniqueCount;
			bTopTailNode.pNode = this->_ptail->pNode;

			//tail의 Next백업
			pbTailNextNode = bTopTailNode.pNode->pNextNode;

			if (nullptr != pbTailNextNode)
			{
				lTailUniqueCount = InterlockedIncrement64((volatile INT64*)&this->_TailUniqueCount);

				if (true == InterlockedCompareExchange128
				(
					(volatile INT64*)_ptail,
					(INT64)lTailUniqueCount,
					(INT64)bTopTailNode.pNode->pNextNode,
					(INT64*)&bTopTailNode
				))
				{
					//InterlockedIncrement64((LONG64*)&this->_UseSize);
				}
				continue;
			}
			//_______________________________________________________________________________________




			//_______________________________________________________________________________________
			// 
			//	Dequeue
			//_______________________________________________________________________________________
			else
			{
				// head 백업
				bTopHeadNode.UniqueCount = this->_phead->UniqueCount;
				bTopHeadNode.pNode = this->_phead->pNode;

				bHeadNextNode = bTopHeadNode.pNode->pNextNode;

				// 사이즈가있다고해서 왔는데 중간에 없는상황
				if (bHeadNextNode == nullptr)
					continue;

				*pOutData = bHeadNextNode->Data;

				if (false == InterlockedCompareExchange128
				(
					(INT64*)this->_phead,
					(INT64)lHeadUniqueCount,
					(INT64)bTopHeadNode.pNode->pNextNode,
					(INT64*)&bTopHeadNode
				))
				{
					// DCAS 실패
					continue;
				}
				else
				{
					// DCAS 성공
					break;
				}
			}
			//____________________________________________________________________
		}

		// CAS128()가 Comp쪽으로 뱉어준 원래노드를 해제
		this->_pFreeList->Free(bTopHeadNode.pNode);

		return true;
	}


public:
	INT64 GetUseSize() { return _UseSize; }
	INT64 GetFreeListAllocSize() { return _pFreeList->GetAllocSize(); }
	INT64 GetFreeListUseSize() { return _pFreeList->GetUseSize(); }

	//Debug
	INT64 GetUniqueCount() { return _phead->UniqueCount; }
	INT64 GetFreeListUniqueCount() { return _pFreeList->GetUniqueCount(); }



private:
	CLockFree_FreeList<NODE>* _pFreeList;
	volatile NODE* _pDummy;
	volatile TopNODE* _phead;
	volatile TopNODE* _ptail;
	volatile INT64	_UseSize;
	volatile INT64 _HeadUniqueCount;
	volatile INT64 _TailUniqueCount;
};


#endif