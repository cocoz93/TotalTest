

#include <thread>
#include "CrashDump.h"
#include "LockFree_FreeList.h"
#include "LockFreeStack.h"
#include "LockFreeQ.h"


#define DATA_VAL			6659
int g_ThreadCount = 0;
int g_DataSize = 0;		// 몇개 데이터를 넣을 것인가
int g_WaitLevel = 0;	// 대기 레벨

volatile unsigned long long g_ThreadIndex = 0;  // 스레드 식별 인덱스

struct TEST_DATA
{
	volatile LONG64 test_Data;
	volatile LONG64 Count;
};

CLockFree_FreeList<TEST_DATA> LockFree_FreeList;
CLockFreeStack<TEST_DATA> LockFreeStack;
CLockFreeQ<TEST_DATA> LockFreeQ;	
unsigned long long* g_LoopCount;				// 테스트 과정 얼마나 돌았는가

HANDLE* HandleArr;
HANDLE hMonitorThread;

////////////////////////////////////////////////////////////////////////////////////
// 
// # LockFreeTest
// # 락프리 결함테스트
//
//
// 1. 데이터 준비(동적 할당), 초기화
// 2. 자료구조에 push
// 3. 약간대기 (다른스레드에서 뽑아가도록 유도)
// 4. 내가 넣은만큼 pop
// 5. 데이터 값 확인
// 6. 뽑은 데이터 값 변경 (인터락 +1 씩)
// 7. 약간 대기
// 8. 변경한 데이터가 그대로인지 확인 (누군가 사용하지는 않는가?)
// 
// 9. delete (참조오류, 댕글링포인터 등 찾기)
// 10.뽑은 수만큼 스택에 다시 넣음
// 11.들어간 수 만큼 큐에서 다시 뽑아서 버림
//
////////////////////////////////////////////////////////////////////////////////////


void LockFree_FreeListTest(void);
UINT WINAPI LockFree_FreeListProc(PVOID arg);
UINT WINAPI FreeListMonitorThread(PVOID arg);

void LockFree_StackTest(void);
UINT WINAPI LockFree_StackProc(PVOID arg);
UINT WINAPI StackMonitorThread(PVOID arg);

UINT WINAPI LockFree_QueueProc(PVOID arg);
UINT WINAPI QueueMonitorThread(PVOID arg);

void LockFree_FreeList_PopTest(void);
void LockFree_Stack_PopTest(void);
void LockFree_Queue_PopTest(void);

void WaitTime(void)
{
	switch (g_WaitLevel)
	{
	case 1:
		YieldProcessor();
		break;
	case 2:
		SwitchToThread();
		break;
	case 3:
		Sleep(0);
		break;
	}
}

////////////////////////////////////////////////////////////////////////////
// 
//		LockFree - FreeList Test
// 
////////////////////////////////////////////////////////////////////////////
void LockFree_FreeListTest(void)
{
	// 테스트 전 초기화 [ 할당 -> 값 초기화 -> 반납 ] (풀내에 데이터 확보)
	//--------------------------------------------------------------
	TEST_DATA** arr = new TEST_DATA * [g_DataSize * g_ThreadCount];

	for (int i = 0; i < g_DataSize * g_ThreadCount; ++i)
		arr[i] = LockFree_FreeList.Alloc();

	for (int i = 0; i < g_DataSize * g_ThreadCount; ++i)
	{
		arr[i]->test_Data = DATA_VAL;
		arr[i]->Count = 0;
	}

	for (int i = 0; i < g_DataSize * g_ThreadCount; ++i)
		LockFree_FreeList.Free(arr[i]);
	//--------------------------------------------------------------


	for (int i = 0; i < g_ThreadCount; ++i)
	{
		HandleArr[i] = (HANDLE)_beginthreadex(NULL, 0, LockFree_FreeListProc, NULL, NULL, NULL);
	}

	hMonitorThread = (HANDLE)_beginthreadex(NULL, 0, FreeListMonitorThread, NULL, NULL, NULL);

	WaitForMultipleObjects(g_ThreadCount, HandleArr, TRUE, INFINITE);
}


UINT WINAPI LockFree_FreeListProc(PVOID arg)
{
	int ThreadIndex = InterlockedIncrement((volatile unsigned long long*)&g_ThreadIndex); // 스레드 인덱스
	TEST_DATA** arr = new TEST_DATA * [g_DataSize * g_ThreadCount];

	while (true)
	{
		WaitTime();

		// 1. Alloc
		// 2. 최초 값 확인
		for (int i = 0; i < g_DataSize; ++i)
		{
			arr[i] = LockFree_FreeList.Alloc();
			if (FALSE == (arr[i]->test_Data == DATA_VAL && arr[i]->Count == 0))
				CCrashDump::Crash();
		}

		// 3. 값 변경
		for (int i = 0; i < g_DataSize; ++i)
		{
			arr[i]->test_Data = DATA_VAL + ThreadIndex; // ThreadIndex값 더하기
			arr[i]->Count = ThreadIndex;
		}
		

		// 4. 약간 대기
		WaitTime();

		// 5. 변경한 값 확인
		for (int i = 0; i < g_DataSize; ++i)
		{
			if (FALSE == (arr[i]->test_Data == (DATA_VAL + ThreadIndex) && arr[i]->Count == ThreadIndex))
				CCrashDump::Crash();
		}

		// 6. 데이터 초기화
		for (int i = 0; i < g_DataSize; ++i)
		{
			arr[i]->test_Data = DATA_VAL;
			arr[i]->Count = 0;
		}

		// 7. 변경한 데이터가 그대로인지 확인 (누군가 사용하지는 않는가?)
		for (int i = 0; i < g_DataSize; ++i)
		{
			if (FALSE == (arr[i]->Count == 0 && arr[i]->test_Data == DATA_VAL))
				CCrashDump::Crash();
		}


		// 8. 데이터 메모리풀에 반환
		for (int i = 0; i < g_DataSize; ++i)
		{
			LockFree_FreeList.Free(arr[i]);
		}

		InterlockedIncrement64((volatile long long*)&g_LoopCount[ThreadIndex - 1]);
	}

	delete[] arr;
	return 0;
}



UINT WINAPI FreeListMonitorThread(PVOID arg)
{
	while (true)
	{
		wprintf(L"-------------------------------------------------------------\n");
		wprintf(L"\n               LockFree - FreeList Test                    \n\n\n");
		wprintf(L"ThreadCount : %d\nDataSize : %d\n", g_ThreadCount, g_DataSize);

		for (int i = 0; i < g_ThreadCount; ++i)
			wprintf(L"[%d] LoopCount : %lld\n",i, g_LoopCount[i]);

		wprintf(L"UseSize / AllocSize : [ %lld / %lld ] \n", LockFree_FreeList.GetUseSize(), LockFree_FreeList.GetAllocSize());
		wprintf(L"UniqueCount(/billion) : %lld\n", LockFree_FreeList.GetUniqueCount() / 1000000000);
		wprintf(L"-------------------------------------------------------------\n");

		Sleep(1000);
	}
}




////////////////////////////////////////////////////////////////////////////
// 
//		LockFree - Stack Test
// 
////////////////////////////////////////////////////////////////////////////
void LockFree_StackTest(void)
{
	// 테스트스레드 생성
	for (int i = 0; i < g_ThreadCount; ++i)
	{
		HandleArr[i] = (HANDLE)_beginthreadex(NULL, 0, LockFree_StackProc, &g_LoopCount[i], NULL, NULL);
	}

	// 모니터스레드 생성
	hMonitorThread = (HANDLE)_beginthreadex(NULL, 0, StackMonitorThread, NULL, NULL, NULL);

	WaitForMultipleObjects(g_ThreadCount, HandleArr, TRUE, INFINITE);
}



UINT WINAPI LockFree_StackProc(PVOID arg)
{
	int ThreadIndex = InterlockedIncrement((volatile unsigned long long*)&g_ThreadIndex); // 스레드 인덱스
	TEST_DATA* arr = new TEST_DATA[g_DataSize];

	// 1. 데이터준비(동적할당), 초기화
	for (int i = 0; i < g_DataSize; ++i)
	{
		arr[i].test_Data = DATA_VAL;
		arr[i].Count = 0;
	}

	while (true)
	{
		// 2. 자료구조에 push
		for (int i = 0; i < g_DataSize; ++i)
			LockFreeStack.push(arr[i]);

		// 3. 약간대기 (다른스레드에서 뽑아가도록 유도)
		WaitTime();

		// 4. 내가 넣은만큼 pop
		for (int i = 0; i < g_DataSize; ++i)
		{
			if (FALSE == LockFreeStack.pop(&arr[i]))
				CCrashDump::Crash();

			//5. 데이터 값 확인
			if (FALSE == ((arr[i].Count == 0) && (arr[i].test_Data == DATA_VAL)))
				CCrashDump::Crash();
		}


		// 6. 뽑은 데이터 값 변경
		for (int i = 0; i < g_DataSize; ++i)
		{
			arr[i].test_Data = DATA_VAL + ThreadIndex; // ThreadIndex값 더하기
			arr[i].Count = ThreadIndex; 
		}

		// 7. 약간 대기
		WaitTime();

		// 8. 변경한 데이터가 그대로인지 확인(누군가 사용하지는 않는가?)
		for (int i = 0; i < g_DataSize; ++i)
		{
			if (FALSE == (arr[i].Count == ThreadIndex && arr[i].test_Data == DATA_VAL + ThreadIndex))
				CCrashDump::Crash();
		}

		//9. 데이터 초기화
		for (int i = 0; i < g_DataSize; ++i)
		{
			arr[i].test_Data = DATA_VAL;
			arr[i].Count = 0;
		}
		
		// 10. 약간 대기
		WaitTime();

		// 11. 변경한 데이터가 그대로인지 확인(누군가 사용하지는 않는가?)
		for (int i = 0; i < g_DataSize; ++i)
		{
			if (FALSE == (arr[i].Count == 0 && arr[i].test_Data == DATA_VAL))
				CCrashDump::Crash();
		}

		InterlockedIncrement64((volatile long long*)&g_LoopCount[ThreadIndex - 1]);
	}

	delete[] arr;

	return 0;
}




UINT WINAPI StackMonitorThread(PVOID arg)
{
	while (true)
	{
		wprintf(L"-------------------------------------------------------------\n");
		wprintf(L"\n                    LockFree - Stack                         \n\n");
		wprintf(L"ThreadCount : %d\nDataSize : %d\n", g_ThreadCount, g_DataSize);
		
		for (int i = 0; i < g_ThreadCount; ++i)
			wprintf(L"[%d] LoopCount : %lld\n", i, g_LoopCount[i]);

		wprintf(L"StackUseSize : %lld\n", LockFreeStack.GetUseSize());
		wprintf(L"StackUniqueCount(/billion) : %lld\n\n", LockFreeStack.GetUniqueCount() / 1000000000);

		wprintf(L"InFreeList Use / Alloc [ %lld / %lld ]\n", LockFreeStack.GetFreeListUseSize(), LockFreeStack.GetFreeListAllocSize());
		wprintf(L"InFreeList UniqueCount(/billion) : %lld\n", LockFreeStack.GetFreeListUniqueCount() / 1000000000);
		wprintf(L"-------------------------------------------------------------\n");
		
		Sleep(1000);
	}
		
	return 0;
}





////////////////////////////////////////////////////////////////////////////
// 
//		LockFree - Queue Test
// 
////////////////////////////////////////////////////////////////////////////
void LockFree_QueueTest(void)
{
	// 테스트스레드 생성
	for (int i = 0; i < g_ThreadCount; ++i)
	{
		HandleArr[i] = (HANDLE)_beginthreadex(NULL, 0, LockFree_QueueProc, &g_LoopCount[i], NULL, NULL);
	}

	// 모니터스레드 생성
	hMonitorThread = (HANDLE)_beginthreadex(NULL, 0, QueueMonitorThread, NULL, NULL, NULL);

	WaitForMultipleObjects(g_ThreadCount, HandleArr, TRUE, INFINITE);
}



UINT WINAPI LockFree_QueueProc(PVOID arg)
{
	int ThreadIndex = InterlockedIncrement((volatile unsigned long long*)&g_ThreadIndex); // 스레드 인덱스
	TEST_DATA* arr = new TEST_DATA[g_DataSize];

	// 1. 데이터준비(동적할당), 초기화
	for (int i = 0; i < g_DataSize; ++i)
	{
		arr[i].test_Data = DATA_VAL;
		arr[i].Count = 0;
	}

	while (true)
	{
		// 2. 자료구조에 push
		for (int i = 0; i < g_DataSize; ++i)
			LockFreeQ.Enqueue(arr[i]);

		// 3. 약간대기 (다른스레드에서 뽑아가도록 유도)
		WaitTime();

		// 4. 내가 넣은만큼 pop
		for (int i = 0; i < g_DataSize; ++i)
		{
			if (FALSE == LockFreeQ.Dequeue(&arr[i]))
				continue;

			//5. 데이터 값 확인
			if (FALSE == ((arr[i].Count == 0) && (arr[i].test_Data == DATA_VAL)))
				CCrashDump::Crash();
		}


		// 5. 뽑은 데이터 값 변경
		for (int i = 0; i < g_DataSize; ++i)
		{
			arr[i].test_Data = DATA_VAL + ThreadIndex; // ThreadIndex값 더하기
			arr[i].Count = ThreadIndex;
		}

		// 6. 약간 대기
		WaitTime();

		// 7. 변경한 데이터가 그대로인지 확인(누군가 사용하지는 않는가 ? )
		for (int i = 0; i < g_DataSize; ++i)
		{
			if (FALSE == (arr[i].Count == ThreadIndex && arr[i].test_Data == DATA_VAL + ThreadIndex))
				CCrashDump::Crash();
		}

		//8. 데이터 초기화
		for (int i = 0; i < g_DataSize; ++i)
		{
			arr[i].test_Data = DATA_VAL; // ThreadIndex값 더하기
			arr[i].Count = 0;
		}

		// 9. 약간 대기
		WaitTime();


		// 10. 변경한 데이터가 그대로인지 확인(누군가 사용하지는 않는가?)
		for (int i = 0; i < g_DataSize; ++i)
		{
			if (FALSE == (arr[i].Count == 0 && arr[i].test_Data == DATA_VAL))
				CCrashDump::Crash();
		}

		InterlockedIncrement64((volatile long long*)&g_LoopCount[ThreadIndex - 1]);
	}

	delete[] arr;
	return 0;
}

#include <conio.h>


UINT WINAPI QueueMonitorThread(PVOID arg)
{
	while (true)
	{
		//SaveProfile
		if (_kbhit() == TRUE)
		{
			if ('`' == _getch())
			{
				printf("\n\n\n\n*******************************************\n");
				printf("\n\t\t Input Crash! \n\n");
				printf("*******************************************\n\n\n\n\n");
				CCrashDump::Crash();
			}
		}

		wprintf(L"-------------------------------------------------------------\n");
		wprintf(L"\n                    LockFree - Queue                         \n\n");
		wprintf(L"ThreadCount : %d\nDataSize : %d\n", g_ThreadCount, g_DataSize);
		
		for (int i = 0; i < g_ThreadCount; ++i)
			wprintf(L"[%d] LoopCount : %lld\n", i, g_LoopCount[i]);

		wprintf(L"QueueUseSize : %lld\n", LockFreeQ.GetUseSize());
		wprintf(L"QueueUniqueCount(/billion) : %lld\n\n", LockFreeQ.GetUniqueCount() / 1000000000);

		wprintf(L"InFreeList Use / Alloc [ %lld / %lld ]\n", LockFreeQ.GetFreeListUseSize(), LockFreeQ.GetFreeListAllocSize());
		wprintf(L"InFreeList UniqueCount(/billion) : %lld\n", LockFreeQ.GetFreeListUniqueCount() / 1000000000);
		wprintf(L"-------------------------------------------------------------\n");

		Sleep(1000);
	}
	return 0;
}

void LockFreeTest()
{
	int TestSelect;
	g_ThreadCount = 0;
	g_DataSize = 0;
	g_WaitLevel = 0;

	wprintf(L"=============================================================\n");
	wprintf(L"                     LockFree Data Check                     \n");
	wprintf(L"                                                             \n");
	wprintf(L"                                                             \n");
	wprintf(L"       1.FreeList        2.Stack         3.Queue             \n");
	wprintf(L"=============================================================\n");
	wscanf_s(L"%d", &TestSelect);

	wprintf(L"ThreadCount : ");
	wscanf_s(L"%d", &g_ThreadCount);
	HandleArr = new HANDLE[g_ThreadCount];
	
	wprintf(L"DataSize : ");
	wscanf_s(L"%d", &g_DataSize);

	wprintf(L"=============================================================\n");
	wprintf(L"                     WaitTime Select\n                        \n");
	wprintf(L"=============================================================\n");
	wprintf(L"1.YieldProcessor       2.SwitchToThread        3.Sleep(0)\n");
	wscanf_s(L"%d", &g_WaitLevel);

	g_LoopCount = new unsigned long long[g_ThreadCount];
	memset(g_LoopCount, 0, sizeof(g_LoopCount) * g_ThreadCount);

	switch (TestSelect)
	{
	case 1:
		LockFree_FreeListTest();
		break;
	case 2:
		LockFree_StackTest();
		break;
	case 3:
		LockFree_QueueTest();
		break;
	}
}

int main(void)
{
	LockFreeTest();
	return 0;
}
