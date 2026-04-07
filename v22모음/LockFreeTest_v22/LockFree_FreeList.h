#pragma once


#ifndef ____LOCKFREE_FREELIST_H____
#define ____LOCKFREE_FREELIST_H____


#include <windows.h>
#include <new>

#define IDENT_VAL 0x6659

template<typename T>
class CLockFree_FreeList
{

	struct NODE
	{
		T		Data;
		//SHORT	IsMine;
		NODE* pNextNode;
	};

	struct TopNODE
	{
		NODE* pNode;
		INT64 UniqueCount;
	} alignas(64);


public:
	explicit CLockFree_FreeList(bool IsPlacementNew = false)
	{
		this->_pTopNode = (TopNODE*)_aligned_malloc(sizeof(TopNODE), 16);
		this->_pTopNode->pNode = nullptr;
		this->_pTopNode->UniqueCount = 0;

		this->_AllocSize = 0;
		this->_UseSize = 0;
		this->_UniqueCount = 0;

		this->_IsPlacementNew = IsPlacementNew;
		hHeap = HeapCreate(NULL, 0, NULL);

		// 저단편화 힙(LFH) 설정
		ULONG HeapInformationValue = 2;
		if (HeapSetInformation(hHeap, HeapCompatibilityInformation,
			&HeapInformationValue, sizeof(HeapInformationValue)));
	}

	virtual ~CLockFree_FreeList()
	{
		NODE* pfNode = nullptr;		//DeleteNode

		while (this->_pTopNode->pNode != nullptr)
		{
			pfNode = this->_pTopNode->pNode;
			this->_pTopNode->pNode = this->_pTopNode->pNode->pNextNode;
			pfNode->Data.~T();

			//delete pfNode;
			HeapFree(hHeap, 0, pfNode);	// HeapFree
		}

		_aligned_free((void*)this->_pTopNode);
	}

public:
	bool Free(T* Data)
	{
		// Free Node
		NODE* fNode = (NODE*)Data;

		// 잘못된 주소가 전달된 경우
		//if (fNode->IsMine != IDENT_VAL)
			//return false;

		// backup TopNode
		TopNODE bTopNode;

		//_______________________________________________________________________________________
		// 
		// DCAS Version. CAS로 가능하다면 DCAS할필요 X
		//_______________________________________________________________________________________
		/*
		//new UniqueCount
		LONG64 nUniqueCount = InterlockedIncrement64((LONG64*)&this->_pTopNode->UniqueCount);
		while (true)
		{
			bTopNode.UniqueCount = this->_pTopNode->UniqueCount;
			bTopNode.pNode = this->_pTopNode->pNode;
			fNode->pNextNode = bTopNode.pNode;
			if (false == InterlockedCompareExchange128
			(
				(LONG64*)this->_pTopNode,
				(LONG64)nUniqueCount,
				(LONG64)fNode,
				(LONG64*)&bTopNode
			))
			{
				// DCAS실패
				continue;
			}
			else
			{
				// DCAS성공
				break;
			}
		}
		*/
		//_______________________________________________________________________________________


		//_______________________________________________________________________________________
		//  
		//	CAS Version
		//_______________________________________________________________________________________
		while (true)
		{
			bTopNode.pNode = this->_pTopNode->pNode;
			fNode->pNextNode = bTopNode.pNode;

			NODE* pNode = (NODE*)InterlockedCompareExchangePointer
			(
				(volatile PVOID*)&this->_pTopNode->pNode,
				(PVOID)fNode,
				(PVOID)bTopNode.pNode
			);

			if (pNode != bTopNode.pNode)
			{
				YieldProcessor();
				continue;
			}
			else
			{
				break;
			}
		}
		//_______________________________________________________________________________________

		// 소멸자 호출
		if (_IsPlacementNew)
			fNode->Data.~T();

		InterlockedDecrement64((volatile INT64*)&this->_UseSize);
		return true;
	}


	T* Alloc()
	{
		INT64   lUniqueCount;	// New UniqCount
		INT64	lUseSize;		// Local UseSize
		INT64	lAllocSize = this->_AllocSize;

		TopNODE bTopNode;		// backup TopNode
		NODE* rNode = nullptr;// return Node


		// UseSize 증가
		lUseSize = InterlockedIncrement64(&this->_UseSize);

		// Node가 있는 경우 pop
		if (lAllocSize >= lUseSize)
		{
			// UniqueCount증가
			lUniqueCount = InterlockedIncrement64(&this->_UniqueCount);

			while (true)
			{
				bTopNode.UniqueCount = this->_pTopNode->UniqueCount;
				bTopNode.pNode = this->_pTopNode->pNode;

				//CAS를 덜 호출하기위함
				if (bTopNode.UniqueCount != this->_pTopNode->UniqueCount)
					continue;

				if (false == InterlockedCompareExchange128
				(
					(volatile INT64*)this->_pTopNode,
					(INT64)lUniqueCount,
					(INT64)bTopNode.pNode->pNextNode,
					(INT64*)&bTopNode
				))
				{
					//CAS 실패
					YieldProcessor();
					continue;
				}
				else
				{
					//CAS 성공
					rNode = bTopNode.pNode;		// 비교노드로 원래있던 노드를 출력해준다.
					break;
				}
			}

			// 생성자 호출
			if (_IsPlacementNew)
				new (&rNode->Data) T;

			// 데이터 반환 (노드반환)
			return (T*)(&(rNode->Data));
		}

		//return Node
		else
		{
			// 1. NewNode
			//rNode = new NODE;
			//rNode->IsMine = IDENT_VAL;
			//rNode->pNextNode = nullptr;

			// 2. HeapCreate
			rNode = (NODE*)HeapAlloc(this->hHeap, FALSE, sizeof(NODE));
			new(&rNode->Data) T;
			rNode->pNextNode = nullptr;

			InterlockedIncrement64(&this->_AllocSize);

			// 데이터 반환 (노드반환)
			return (T*)(&(rNode->Data));
		}
	}


public:
	__forceinline INT64 GetUseSize() { return _UseSize; }
	__forceinline INT64 GetAllocSize() { return _AllocSize; }

	//Debug
	__forceinline INT64 GetUniqueCount() { return _pTopNode->UniqueCount; }

private:
	TopNODE* _pTopNode;		//_allinge_malloc()
	bool	_IsPlacementNew;

	INT64	_UseSize;		//실제 바깥에서 사용되고있는 노드(malloc노드)
	INT64 _AllocSize;		//바깥으로 Alloc한 노드사이즈	
	INT64 _UniqueCount;
	HANDLE hHeap;
};

#endif