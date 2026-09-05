/////////////////////////////////////////////////////////
// header
/////////////////////////////////////////////////////////

#pragma once

#include <atomic>
#include <cstdint>
#include "LockFreeConfig.h"

// 기본 직렬화 버퍼 크기 — 현재 프로토콜 최대 패킷(~1034B)을 수용할 수 있으면 충분
// 값 자체에 특별한 성능적 근거는 없음. 대형 패킷 추가 시 MAX_PACKET_SIZE와 함께 확장할 것
static constexpr int MSG_DEFAULT_SIZE = 1460;
static constexpr int HEADER_SIZE = 2;


class CSerialBuffer
{
public:
	static LockFree::CExternalTlsFreeList<CSerialBuffer>* _TlsMsgFreeList;

public:
	static CSerialBuffer* Alloc();
	void AddRef();
	void AddRef(int64_t count);   // 배치 AddRef — 브로드캐스트 타겟 수만큼 1회 (원자연산 N→1)
	void SubRef();

private:
	// 수명 회수는 SubRef()로만 — RefCount=0일 때 SubRef 내부에서만 호출 (외부 직접 호출 금지)
	static void Free(CSerialBuffer* msg);

public:

	// 쓰기 완료 후 호출. 봉인 이후 SetData/GetData 차단
	// 멀티스레드 브로드캐스트 시 내부 상태(_front, _rear) 보호 목적
	void Seal();


public:
	//private:
	explicit CSerialBuffer();
	explicit CSerialBuffer(int BufferSize);
	virtual	~CSerialBuffer();

	CSerialBuffer(const CSerialBuffer&) = delete;
	CSerialBuffer(CSerialBuffer&&) = delete;

public:
	void Initialize(int BufferSize);		// 메모리풀 할당
	void Release(void); //Msg 해제
	void Clear(void);   //Msg 초기화 (메모리풀로 사용할때 호출)

public:
	int	GetBufferSize(void) { return _BufferSize; }

	//헤더를 제외한, 실제 컨텐츠쪽에서 사용하고있는 사이즈
	int	GetDataSize(void) { return _DataSize; }
	char* GetReadBufferPtr(void) { return _Buff + HEADER_SIZE + _front; }
	char* GetWriteBufferPtr(void) { return _Buff + HEADER_SIZE + _rear; }
	int	MoveWritePos(int size);
	int	MoveReadPos(int size);

	//헤더부분 접근금지
public:
	char* GetHeaderBufferPtr(void) { return _Buff; }
	char* GetPayloadBufferPtr(void) { return _Buff + HEADER_SIZE; }

public:
	bool IsFull(int size) { return _DataSize + size > _BufferSize - HEADER_SIZE; }
	bool IsEmpty(int size) { return size > _DataSize; }
	bool IsSealed() { return _Sealed; }

	//헤더세팅은 바깥에서 막기위함
public:
	//bool Checkheader();


public:
	CSerialBuffer& operator = (const CSerialBuffer& SrcMsg);


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
	//CSerialBuffer& operator >> (char* Value);

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
	char*		_Buff;
	int			_BufferSize;
	int			_DataSize;
	int			_front = 0;
	int			_rear = 0;
	bool		_Sealed = false;

	// 여러 워커스레드에서 동시 Interlocked 접근 → 별도 캐시라인으로 격리하여 false sharing 방지
public:
	alignas(64) std::atomic<int64_t> _RefCount;
};
