#pragma comment(lib, "DbgHelp")	//MiniDumpWriteDump

#include <stdio.h>
#include <windows.h>
#include <Psapi.h>		//PROCESS_MEMORY_COUNTERS
#include <DbgHelp.h>	//_MINIDUMP_EXCEPTION_INFORMATION
#include <crtdbg.h>		//_CrtSetReportMode


class CCrashDump
{
public:
	explicit CCrashDump()
	{
		_DumpCount = 0;

		//밑 구문들은 CRT오류 메시지표시를 중단하기 위함.
		//우리는 덤프를 빼고 프로세스를 종료시켜야 한다.

		//-------------------------------------------------------------------------------

		// INVALIDE_PARAMETER 에러핸들러를 우리가 캐치
		// ex) CRL함수에 잘못된 인자전달 (매개변수 가변인자 오류, NULL이 들어갈수 없는곳에 NULL)

		_invalid_parameter_handler oldHandler;
		oldHandler = _set_invalid_parameter_handler(myInvalidParameterHandler);

		// pure virtual function called 에러핸들러를 우리가 캐치
		_set_purecall_handler(MyPurecallHandler);


		//위 네줄은 경험한 적은 없으나 만약을 위함
		_CrtSetReportMode(_CRT_WARN, 0);
		_CrtSetReportMode(_CRT_ASSERT, 0);
		_CrtSetReportMode(_CRT_ERROR, 0);
		_CrtSetReportHook(_custom_Report_hook);


		// 핸들링되지 않은 모든 예외를 우리쪽으로 캐치
		// ex) Throw 던졌는데 Catch존재하지않음, 메모리참조 오류, 모든예외를 받는 경우 등
		// 원래는 catch를 못받는 경우 메인까지 튀어나와 SHE로 예외발생
		SetUnhandledExceptionFilter(MyExceptionFilter);


		//-------------------------------------------------------------------------------
	}


	static void Crash(void)
	{
		//if (FALSE == CMyLog::FileSaveLog())
		//	return;

		int* p = nullptr;
		*p = 0;
	}



	/*
	우리가 정의한 MyExceptionFilter는 SetHandlerDump()함수 내부에서,
	API함수인 SetUnhandledExceptionFilter()의 매개변수로 전달될 함수.

	이 함수의 매개변수 EXCEPTION_POINTERS은 규칙을 따른것.
	ExceptionFilter라는 구조체의 포인터를 여기서 받으면 된다.
	그럼 예외발생시 이 인자로 자동으로 들어올 것이다.
	우리는 여기서 덤프를 여기서 뺀다.
	*/
	static LONG WINAPI MyExceptionFilter(__in PEXCEPTION_POINTERS pExceptionPointer)
	{
		SYSTEMTIME NowTime;

		long DumpCount = InterlockedIncrement(&_DumpCount);


		//----------------------------------------------------------
		// 현재 날짜와 시간을 알아온다.
		//----------------------------------------------------------
		HANDLE hProcess = 0;
		PROCESS_MEMORY_COUNTERS pmc;
		int WorkingMemory = 0;

		hProcess = GetCurrentProcess();
		if (hProcess == NULL)
			return 0;

		if (GetProcessMemoryInfo(hProcess, &pmc, sizeof(pmc)))
		{
			WorkingMemory = (int)(pmc.WorkingSetSize / 1024 / 1024);
			//이부분. 생략해도 상관없음. 불필요한 코드가 될수있음.

		}
		CloseHandle(hProcess);



		//----------------------------------------------------------
		// 현재 날짜와 시간을 알아온다.
		//----------------------------------------------------------
		WCHAR filename[MAX_PATH];

		GetLocalTime(&NowTime);
		wsprintf(filename, L"Dump_%d%02d%02d_%02d.%02d.%02d_%d_%d.dmp",
			NowTime.wYear, NowTime.wMonth, NowTime.wDay, NowTime.wHour, NowTime.wMinute, NowTime.wSecond, DumpCount);

		wprintf(L"\n\n\n!!! Crash Error!!! %d.%d.%d/%d:%d:%d\n", NowTime.wYear, NowTime.wMonth, NowTime.wDay, NowTime.wHour, NowTime.wMinute, NowTime.wSecond);
		wprintf(L"Now Save dump file...\n");

		HANDLE hDumpFile = CreateFile
		(
			filename,
			GENERIC_WRITE,
			FILE_SHARE_WRITE,		//쓰기모드
			NULL,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, NULL
		);


		if (hDumpFile != INVALID_HANDLE_VALUE)
		{
			//덤프파일 정보 설정
			_MINIDUMP_EXCEPTION_INFORMATION MinidumpExceptionInformation;

			MinidumpExceptionInformation.ThreadId = ::GetCurrentThreadId();
			MinidumpExceptionInformation.ExceptionPointers = pExceptionPointer;	 //인자로 들어온 예외포인터
			MinidumpExceptionInformation.ClientPointers = FALSE;
			//msdn상에서는 외부디버거에서 해당 덤프를 뺄때 설정이라고 명시되어있음.
			//TRUE/FALSE에 대한 차이점을 찾지못함.


			/*
			MiniDumpWriteDump는 실질적으로 메인이 되는 함수로,
			호출 시 전달된 파일(핸들)을 대상으로 write가 시작된다.
			*/
			MiniDumpWriteDump
			(
				GetCurrentProcess(),
				GetCurrentProcessId(),
				hDumpFile, //File 핸들.						 
				MiniDumpWithFullMemory,	//_MINIDUMP_TYPE. 풀덤프로 지정.
				&MinidumpExceptionInformation, //우리가 정의한 미니덤프의 예외정보. 
				NULL,
				NULL
			);
			CloseHandle(hDumpFile);
			wprintf(L"CrashDumpSaveFinish!");
		}

		return EXCEPTION_EXECUTE_HANDLER;
		//파일에 덤프를 모두 저장 후 리턴.
		//해당값을 리턴해 예외처리가 끝났다고 알려, 예외창이 뜨는것을 막는다.
	}





	static void myInvalidParameterHandler(const wchar_t* expression, const wchar_t* function, const wchar_t* file, unsigned int line, uintptr_t pReserved)
	{
		Crash();
	}

	static int _custom_Report_hook(int ireposttype, char* message, int* returnval)
	{
		Crash();
		return true;
	}

	static void MyPurecallHandler(void)
	{
		Crash();
	}

private:
	static long _DumpCount;
};


//long CCrashDump::_DumpCount;
//CCrashDump CrashDump;





