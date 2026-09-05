/////////////////////////////////////////////////////////
// header — CSerialBuffer 수정본
/////////////////////////////////////////////////////////

#pragma once

#include <atomic>
#include <cstdint>
#include "LockFreeConfig.h"

//=============================================================================
// [수정본 — 원본이 아님]
//
// SerialBuffer.h / .cpp (MMOServer 원본 사본) 에서 발견된 결함을 반영한 변형.
// 원본 사본은 손대지 않는다 — 사본이 갈라지면 테스트가 서버가 돌리지 않는
// 코드를 검증하게 되고, Phase 4 가 기록하는 "실제 서버 상태"도 사라진다.
//
// 같은 테스트 14종을 두 버전 모두에 돌릴 수 있도록 namespace 로 분리했다.
//   빌드:  기본        → 원본 사본
//          -DUSE_FIXED_SERIALBUFFER → 이 수정본
//
// [반영한 수정]
//   (1) IsFull 기준        — _DataSize → _rear, 음수·용량 가드 추가
//                            → 재사용 시 버퍼 밖 쓰기 / 음수 길이 통과 /
//                              극소 버퍼의 "우연한" 안전 을 한 번에 해소
//   (2) 버퍼 수명          — _Buff 기본 초기화, Release() 가 null 로 만들고
//                            Initialize() 가 이전 버퍼를 해제
//   (3) operator= 복사 길이 — HEADER_SIZE + _DataSize → HEADER_SIZE + _rear
//
// [진단으로만 처리한 것 — 자동 복구가 불가능한 것들]
//   (4) 소유권 초과 반납    — 이미 잘못 호출된 뒤라 되돌릴 수 없다.
//                            조용히 새는 대신 플래그로 검출 가능하게 한다.
//   (5) operator= 조용한 실패 — operator= 는 반환값으로 알릴 수 없다.
//                            플래그를 세워 호출부가 확인할 수 있게 한다.
//   → 둘 다 원본에 없던 API 다. 실제 서버에 반영할 때는 assert 나 로그로
//     바꾸는 편이 낫다 (테스트에서는 죽지 않아야 하므로 플래그를 썼다).
//=============================================================================

namespace Fixed
{

static constexpr int MSG_DEFAULT_SIZE = 1460;
static constexpr int HEADER_SIZE = 2;


class CSerialBuffer
{
public:
	static LockFree::CExternalTlsFreeList<CSerialBuffer>* _TlsMsgFreeList;

public:
	static CSerialBuffer* Alloc();
	void AddRef();
	void AddRef(int64_t count);
	void SubRef();

private:
	static void Free(CSerialBuffer* msg);

public:
	void Seal();

public:
	explicit CSerialBuffer();
	explicit CSerialBuffer(int BufferSize);
	virtual	~CSerialBuffer();

	CSerialBuffer(const CSerialBuffer&) = delete;
	CSerialBuffer(CSerialBuffer&&) = delete;

public:
	void Initialize(int BufferSize);
	void Release(void);
	void Clear(void);

public:
	int	GetBufferSize(void) { return _BufferSize; }
	int	GetDataSize(void) { return _DataSize; }
	char* GetReadBufferPtr(void) { return _Buff + HEADER_SIZE + _front; }
	char* GetWriteBufferPtr(void) { return _Buff + HEADER_SIZE + _rear; }
	int	MoveWritePos(int size);
	int	MoveReadPos(int size);

public:
	char* GetHeaderBufferPtr(void) { return _Buff; }
	char* GetPayloadBufferPtr(void) { return _Buff + HEADER_SIZE; }

public:
	// [수정 1] 원본: return _DataSize + size > _BufferSize - HEADER_SIZE;
	//
	//   · size < 0 거부       — (short)strlen 이 32767 초과에서 음수로 잘리면
	//                           그 음수가 그대로 이 식에 들어와 가드를 통과했다.
	//                           MoveWritePos(-1) 도 같은 경로로 통과했다.
	//   · capacity <= 0 거부  — 버퍼가 헤더보다 작으면 우변이 음수가 되는데,
	//                           좌변이 0 이상이라 "우연히" 막히고 있었다.
	//   · _rear 기준          — 읽기는 _DataSize 만 줄이고 _rear 는 그대로다.
	//                           _DataSize 기준이면 Clear 없이 재사용할 때
	//                           "여유 있음" 판정과 실제 남은 공간이 어긋나
	//                           버퍼 밖에 쓰게 된다.
	//
	//   실사용 경로는 Alloc 직후(_DataSize == _rear == 0) 한 번만 판정하므로
	//   이 변경으로 동작이 달라지지 않는다. 달라지는 것은 Clear 없는 재사용뿐이고,
	//   그게 막고 싶은 경우다.
	bool IsFull(int size)
	{
		if (size < 0)
			return true;

		const int capacity = _BufferSize - HEADER_SIZE;
		if (capacity <= 0)
			return true;

		return _rear + size > capacity;
	}

	// [수정 1-b] 음수 거부 — MoveReadPos(-1) 이 else 분기로 빠져
	//   _DataSize 를 늘리는(없던 데이터가 생기는) 경로를 막는다.
	bool IsEmpty(int size)
	{
		if (size < 0)
			return true;

		return size > _DataSize;
	}

	bool IsSealed() { return _Sealed; }

public:
	CSerialBuffer& operator = (const CSerialBuffer& SrcMsg);

	// [수정 5] operator= 는 반환값으로 실패를 알릴 수 없다.
	//   대상 버퍼가 작아 복사를 건너뛰었는지 호출부가 확인할 수 있게 한다.
	bool HasCopyFailed() const { return _CopyFailed; }
	void ClearCopyFailed() { _CopyFailed = false; }

	// [수정 4] AddRef 보다 SubRef 를 많이 부르면 회수가 영영 일어나지 않는다.
	//   이미 벌어진 뒤라 되돌릴 수 없으므로, 조용히 새는 대신 검출만 가능하게 한다.
	bool HasRefUnderflow() const { return _RefUnderflow.load(); }

	//Input
public:
	CSerialBuffer& operator << (const char* Value);
	CSerialBuffer& operator << (const wchar_t* Value);

	CSerialBuffer& operator << (uint8_t Value);
	CSerialBuffer& operator << (char Value);

	CSerialBuffer& operator << (short Value);
	CSerialBuffer& operator << (uint16_t Value);

	CSerialBuffer& operator << (int Value);
	CSerialBuffer& operator << (uint32_t Value);

	CSerialBuffer& operator << (int64_t Value);
	CSerialBuffer& operator << (uint64_t Value);

	CSerialBuffer& operator << (float Value);
	CSerialBuffer& operator << (double Value);

	//Output
public:
	CSerialBuffer& operator >> (uint8_t& Value);
	CSerialBuffer& operator >> (char& Value);

	CSerialBuffer& operator >> (short& Value);
	CSerialBuffer& operator >> (uint16_t& Value);

	CSerialBuffer& operator >> (int& Value);
	CSerialBuffer& operator >> (uint32_t& Value);

	CSerialBuffer& operator >> (int64_t& Value);
	CSerialBuffer& operator >> (uint64_t& Value);

	CSerialBuffer& operator >> (float& Value);
	CSerialBuffer& operator >> (double& Value);

public:
	int	GetData(char* Dest, int size);
	int PeekData(char* Dest, int size);
	int	SetData(char* Src, int size);


private:
	// [수정 2] 기본 초기화 — Initialize() 가 이전 버퍼를 해제하려면
	//   첫 호출에서 _Buff 가 유효하거나 nullptr 이어야 한다.
	char*		_Buff = nullptr;
	int			_BufferSize = 0;
	int			_DataSize = 0;
	int			_front = 0;
	int			_rear = 0;
	bool		_Sealed = false;
	bool		_CopyFailed = false;

public:
	alignas(64) std::atomic<int64_t> _RefCount;

private:
	std::atomic<bool> _RefUnderflow{ false };
};

}   // namespace Fixed
