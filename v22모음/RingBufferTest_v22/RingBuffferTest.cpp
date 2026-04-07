


#include <windows.h>
#include <iostream>
#include <time.h>
#include <process.h>

#include "RingBuffer.h"

// config
char TestStr[] = { "1234567890 abcdefghijklmnopqrstuvwxyz 1234567890 abcdefghijklmnopqrstuvwxyz 12345" };
const int SleepTime = 10; // ms

const int strsize = sizeof(TestStr);
CRingBuffer TestQ;
CRITICAL_SECTION cs;


UINT WINAPI EnqueueThread(void* arg)
{
    srand(unsigned(time(NULL) ^ (unsigned)EnqueueThread));
    int EnquePos = 0;

    while (true)
    {
        EnterCriticalSection(&cs);
        int EnqueSize = (rand() % 82) + 1;

        // 링버퍼에 다들어갈수있는 데이터크기인가?
        if (TestQ.GetFreeSize() < EnqueSize)
            EnqueSize = TestQ.GetFreeSize();

        // 테스트문자열이 Enque하는 순서는 지켜져야 하므로, Pos를 둔다.
        if (EnqueSize + EnquePos > sizeof(TestStr))
            EnqueSize = sizeof(TestStr) - EnquePos;

        TestQ.Enqueue(TestStr + EnquePos, EnqueSize);

		// 순서지켜서 Enqueue 하기위한 Pos
        EnquePos += EnqueSize;
		EnquePos %= sizeof(TestStr);  // TestStr[81] => 값0, 사이즈1 이 들어갈수 있음. 상관없다.

        LeaveCriticalSection(&cs);
        Sleep(SleepTime);
    }
    return 0;
}

UINT WINAPI DeuqeueThread(void* arg)
{
    srand(unsigned(time(NULL) ^ (unsigned)DeuqeueThread));

    while (true)
    {
        EnterCriticalSection(&cs);

        // Dequeue할 사이즈가 없다면 진행X
        if (0 == TestQ.GetUseSize())
        {
			LeaveCriticalSection(&cs);  
            continue;
        }

        // Dequeue는 랜덤사이즈
        int DequeSize = (rand() % 82) + 1;
        if (TestQ.GetUseSize() < DequeSize)
            DequeSize = TestQ.GetUseSize();

        // 콘솔 출력용 버퍼
        char DebugArr[strsize + 1];
        DebugArr[strsize] = 0;
        int OutDequeSize = TestQ.Dequeue(DebugArr, DequeSize);

        for (int i = 0; i < OutDequeSize; ++i)
             std::cout << DebugArr[i];

        LeaveCriticalSection(&cs);
        Sleep(SleepTime + 1);
    }
    return 0;
}



////////////////////////////////////////////////////////////////////////////////////
// 
// * 링버퍼 테스트
// 
// 문자열에 공백이나 숫자,영문 등의 패턴이 있는 문자열을 랜덤한 길이로 
// Enqueue, Dequeue를 반복하면서 데이터가 깨지는지 확인한다.
// 
// 테스트 스트링
// 1234567890 abcdefghijklmnopqrstuvwxyz 1234567890 abcdefghijklmnopqrstuvwxyz 12345
// 1234567890 abcdefghijklmnopqrstuvwxyz 1234567890 abcdefghijklmnopqrstuvwxyz 12345
// ( 큐가 잘못 되었다면  줄이 틀어짐 ) = > 콘솔창 넓이 81
// 
// 
// * 조건
// 
// 화면에 출력되는 텍스트는 무조건 큐에서 뽑은 데이터를 출력.
// \n 줄바꿈 금지
// 큐 버퍼를 작은 사이즈 (100 ~ 1000)  로 다양하게 테스트 후,  큰 사이즈도 테스트
// 테스트는 시간 최대한 확보하기
// 뽑기 전 Peek 를 하여  Peek 와 dequeue 이 같은 값이 나왔는지 memcmp 하여 비교 테스트.
//
////////////////////////////////////////////////////////////////////////////////////

int main()
{
    InitializeCriticalSection(&cs);
    HANDLE EnqueThread = (HANDLE)_beginthreadex(NULL, 0, EnqueueThread, NULL, NULL, NULL);
    HANDLE DequeThread = (HANDLE)_beginthreadex(NULL, 0, DeuqeueThread, NULL, NULL, NULL);

    HANDLE ThreadARR[2] = { EnqueThread, DequeThread };

    //Q사이즈 모니터링
    //while (true) 
    //{
    //    std::cout << TestQ.GetUseSize() << '\n';
    //    Sleep(20);
    //}

    WaitForMultipleObjects(2, ThreadARR, TRUE, INFINITE);
    DeleteCriticalSection(&cs);

    return 0;
}


