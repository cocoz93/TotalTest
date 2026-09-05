//////////////////////////////////////////////////////////////////////////
// CPP — CSerialBuffer 수정본 (원본이 아님)
//   SerialBuffer.cpp 에서 파생. 변경한 곳은 [수정 N] 주석으로 표시했다.
//////////////////////////////////////////////////////////////////////////

#include "SerialBuffer_Fixed.h"
#include "Platform/Platform.h"   // memcpy_s·wcslen 등 CRT 차이 흡수

namespace Fixed
{

//CFreeList<CSerialBuffer> CSerialBuffer::_MsgFreeList;
//CLockFree_FreeList<CSerialBuffer>* CSerialBuffer::_MsgFreeList;
LockFree::CExternalTlsFreeList<CSerialBuffer>* CSerialBuffer::_TlsMsgFreeList;

CSerialBuffer* CSerialBuffer::Alloc()
{
	//profile.Begin((WCHAR*)L"CSerialBuffer::Alloc");
	//________________________________________________________


	//0. new사용 
	//CSerialBuffer* msg = new CSerialBuffer;

	//1. LockFree-FreeList사용
	//CSerialBuffer* msg = _MsgFreeList->Alloc();

	//2. Tls-LockFree-FreeList사용
	CSerialBuffer* msg = _TlsMsgFreeList->Alloc();
	//________________________________________________________

	msg->Clear();   // Clear()가 RefCount=1로 초기화 → 반환 버퍼는 "생성자 소유권 1개" 보유
	//profile.End((WCHAR*)L"CSerialBuffer::Alloc");

	return msg;
}



void CSerialBuffer::Free(CSerialBuffer* msg)
{
	//profile.Begin((WCHAR*)L"CSerialBuffer::Free");
	//________________________________________________________

	//0. new사용 
	//delete msg;

	//1. LockFree-FreeList사용r
	//_MsgFreeList->Free(msg);

	//2. Tls-LockFree-FreeList사용
	_TlsMsgFreeList->Free(msg);
	//________________________________________________________


	//profile.End((WCHAR*)L"CSerialBuffer::Free");
}



// 참조 카운트만 증가. Seal()과 독립적으로 동작
void CSerialBuffer::AddRef()
{
	_RefCount.fetch_add(1);
}

// 배치 AddRef — 타겟별 AddRef를 호출당 1회로 압축 (BroadcastAroundSector)
void CSerialBuffer::AddRef(int64_t count)
{
	_RefCount.fetch_add(count);
}

// Alloc() → operator<< → Seal() → AddRef(N) → WSASend × N → SubRef()
void CSerialBuffer::Seal()
{
	_Sealed = true;
}



void CSerialBuffer::SubRef()
{
	const int64_t prev = _RefCount.fetch_sub(1);
	if (prev == 1)
	{
		Free(this);
		return;   // 이후 this 접근 금지
	}

	// [수정 4] AddRef 보다 SubRef 를 많이 부른 경우.
	//   이미 0 이하로 내려간 뒤라 되돌릴 수 없다. 조용히 새는 대신 검출만 가능하게 한다.
	//   (실제 서버에서는 assert 나 로그가 낫다 — 테스트는 죽으면 안 되므로 플래그)
	if (prev <= 0)
		_RefUnderflow.store(true);
}



CSerialBuffer::CSerialBuffer()
{
	Initialize(MSG_DEFAULT_SIZE);
}

CSerialBuffer::CSerialBuffer(int BufferSize)
{
	Initialize(BufferSize);
}

CSerialBuffer::~CSerialBuffer()
{
	Release();
}

void CSerialBuffer::Initialize(int BufferSize)
{
	// [수정 2] 이전 버퍼를 해제하지 않고 덮어쓰면 누수된다.
	//   _Buff 는 헤더에서 nullptr 로 초기화되므로 첫 호출에서도 안전하다.
	delete[] _Buff;
	_Buff = new char[BufferSize];
	_BufferSize = BufferSize;

	this->Clear();
}

void CSerialBuffer::Release(void)
{
	// [수정 2] null 로 만들지 않으면 Release() 후 소멸 시 이중 해제가 된다.
	delete[] _Buff;
	_Buff = nullptr;
}

void CSerialBuffer::Clear(void)
{
	this->_front = 0;
	this->_rear = 0;
	this->_DataSize = 0;
	this->_RefCount = 1;   // 생성자 소유권 — Alloc()이 반환하는 버퍼는 항상 RefCount=1 (사용처가 SubRef/Free로 회수)
	this->_Sealed = false;
	this->_CopyFailed = false;
	this->_RefUnderflow.store(false);
}

int CSerialBuffer::MoveWritePos(int size)
{
	//어차피 바깥에서 셋팅하고 Pos를 이동시키기문에,
	//여기서 공간이 부족할까봐 AddAlloc하는 로직이 들어가지않아도 된다
	if (IsFull(size))
		return 0;

	_rear += size;
	_DataSize += size;

	return size;
}

int CSerialBuffer::MoveReadPos(int size)
{
	// [수정 1-c] 이 함수는 IsEmpty 를 거치지 않고 직접 비교하므로 음수 가드가 따로 필요하다.
	//   size < 0 이면 (_front + size > _rear) 가 성립하지 않아 else 로 빠지고,
	//   _DataSize -= (음수) 가 되어 없던 데이터가 생긴다.
	if (size < 0)
		return 0;

	// 만약 size만큼 이동하려했는데, 
	// 이동할 front가 가진 데이터보다 더 뒤로 가는경우
	// DataSize에서 멈추도록 한다.
	if (_front + size > _rear)
	{
		int moved = _DataSize;
		_front += _DataSize;
		_DataSize = 0;
		return moved;
	}
	else
	{
		_front += size;
		_DataSize -= size;
		return size;
	}
}

//딱히 필요없음
//bool CSerialBuffer::Checkheader()
//{
//	//short Len = 0;
//	//memcpy_s(&Len, HEADER_SIZE, _Buff, HEADER_SIZE);
//	if ((SHORT)(*(SHORT*)(this->_Buff)) == PAYLOAD_SIZE)
//		return TRUE;
//	else return FALSE;
//}



CSerialBuffer& CSerialBuffer::operator=(const CSerialBuffer& SrcMsg)
{
	if (this == &SrcMsg)
		return *this;

	// [수정 3] 유효 데이터 구간의 끝은 HEADER_SIZE + _rear 다.
	//   _DataSize 기준으로 복사하면 _front > 0 일 때 뒷부분이 빠진다.
	int srcTotalSize = HEADER_SIZE + SrcMsg._rear;
	if (srcTotalSize > _BufferSize)
	{
		// [수정 5] operator= 는 반환값으로 실패를 알릴 수 없다 → 플래그
		_CopyFailed = true;
		return *this;
	}

	memcpy_s(_Buff, _BufferSize, SrcMsg._Buff, srcTotalSize);
	_CopyFailed = false;
	_DataSize = SrcMsg._DataSize;
	_front = SrcMsg._front;
	_rear = SrcMsg._rear;
	_Sealed = SrcMsg._Sealed;
	return *this;
}




CSerialBuffer& CSerialBuffer::operator<<(const char* Value)
{
	short Len = (short)strlen(Value);
	if (IsFull(sizeof(Len) + Len))
		return *this;
	SetData((char*)&Len, sizeof(Len));
	SetData((char*)Value, Len);
	return *this;
}

CSerialBuffer& CSerialBuffer::operator<<(const wchar_t* Value)
{
	short Len = (short)(wcslen(Value) * sizeof(wchar_t));
	if (IsFull(sizeof(Len) + Len))
		return *this;
	SetData((char*)&Len, sizeof(Len));
	SetData((char*)Value, Len);
	return *this;
}

CSerialBuffer& CSerialBuffer::operator<<(uint8_t Value)
{
	if (_Sealed || IsFull(sizeof(Value))) return *this;
	*(uint8_t*)(_Buff + HEADER_SIZE + _rear) = Value;
	_rear += sizeof(Value);
	_DataSize += sizeof(Value);
	return *this;
}

CSerialBuffer& CSerialBuffer::operator<<(char Value)
{
	if (_Sealed || IsFull(sizeof(Value))) return *this;
	*(_Buff + HEADER_SIZE + _rear) = Value;
	_rear += sizeof(Value);
	_DataSize += sizeof(Value);
	return *this;
}

CSerialBuffer& CSerialBuffer::operator<<(short Value)
{
	if (_Sealed || IsFull(sizeof(Value))) return *this;
	*(short*)(_Buff + HEADER_SIZE + _rear) = Value;
	_rear += sizeof(Value);
	_DataSize += sizeof(Value);
	return *this;
}

CSerialBuffer& CSerialBuffer::operator<<(uint16_t Value)
{
	if (_Sealed || IsFull(sizeof(Value))) return *this;
	*(uint16_t*)(_Buff + HEADER_SIZE + _rear) = Value;
	_rear += sizeof(Value);
	_DataSize += sizeof(Value);
	return *this;
}

CSerialBuffer& CSerialBuffer::operator<<(int Value)
{
	if (_Sealed || IsFull(sizeof(Value))) return *this;
	*(int*)(_Buff + HEADER_SIZE + _rear) = Value;
	_rear += sizeof(Value);
	_DataSize += sizeof(Value);
	return *this;
}

CSerialBuffer& CSerialBuffer::operator<<(uint32_t Value)
{
	if (_Sealed || IsFull(sizeof(Value))) return *this;
	*(uint32_t*)(_Buff + HEADER_SIZE + _rear) = Value;
	_rear += sizeof(Value);
	_DataSize += sizeof(Value);
	return *this;
}

CSerialBuffer& CSerialBuffer::operator<<(float Value)
{
	if (_Sealed || IsFull(sizeof(Value))) return *this;
	*(float*)(_Buff + HEADER_SIZE + _rear) = Value;
	_rear += sizeof(Value);
	_DataSize += sizeof(Value);
	return *this;
}

CSerialBuffer& CSerialBuffer::operator<<(int64_t Value)
{
	if (_Sealed || IsFull(sizeof(Value))) return *this;
	*(int64_t*)(_Buff + HEADER_SIZE + _rear) = Value;
	_rear += sizeof(Value);
	_DataSize += sizeof(Value);
	return *this;
}

CSerialBuffer& CSerialBuffer::operator<<(uint64_t Value)
{
	if (_Sealed || IsFull(sizeof(Value))) return *this;
	*(uint64_t*)(_Buff + HEADER_SIZE + _rear) = Value;
	_rear += sizeof(Value);
	_DataSize += sizeof(Value);
	return *this;
}

CSerialBuffer& CSerialBuffer::operator<<(double Value)
{
	if (_Sealed || IsFull(sizeof(Value))) return *this;
	*(double*)(_Buff + HEADER_SIZE + _rear) = Value;
	_rear += sizeof(Value);
	_DataSize += sizeof(Value);
	return *this;
}




CSerialBuffer& CSerialBuffer::operator>>(uint8_t& Value)
{
	if (_Sealed || IsEmpty(sizeof(Value))) { Value = 0; return *this; }
	Value = *(uint8_t*)(_Buff + HEADER_SIZE + _front);
	_front += sizeof(Value);
	_DataSize -= sizeof(Value);
	return *this;
}

CSerialBuffer& CSerialBuffer::operator>>(char& Value)
{
	if (_Sealed || IsEmpty(sizeof(Value))) { Value = 0; return *this; }
	Value = *(_Buff + HEADER_SIZE + _front);
	_front += sizeof(Value);
	_DataSize -= sizeof(Value);
	return *this;
}

CSerialBuffer& CSerialBuffer::operator>>(short& Value)
{
	if (_Sealed || IsEmpty(sizeof(Value))) { Value = 0; return *this; }
	Value = *(short*)(_Buff + HEADER_SIZE + _front);
	_front += sizeof(Value);
	_DataSize -= sizeof(Value);
	return *this;
}

CSerialBuffer& CSerialBuffer::operator>>(uint16_t& Value)
{
	if (_Sealed || IsEmpty(sizeof(Value))) { Value = 0; return *this; }
	Value = *(uint16_t*)(_Buff + HEADER_SIZE + _front);
	_front += sizeof(Value);
	_DataSize -= sizeof(Value);
	return *this;
}

CSerialBuffer& CSerialBuffer::operator>>(int& Value)
{
	if (_Sealed || IsEmpty(sizeof(Value))) { Value = 0; return *this; }
	Value = *(int*)(_Buff + HEADER_SIZE + _front);
	_front += sizeof(Value);
	_DataSize -= sizeof(Value);
	return *this;
}

CSerialBuffer& CSerialBuffer::operator>>(uint32_t& Value)
{
	if (_Sealed || IsEmpty(sizeof(Value))) { Value = 0; return *this; }
	Value = *(uint32_t*)(_Buff + HEADER_SIZE + _front);
	_front += sizeof(Value);
	_DataSize -= sizeof(Value);
	return *this;
}

CSerialBuffer& CSerialBuffer::operator>>(float& Value)
{
	if (_Sealed || IsEmpty(sizeof(Value))) { Value = 0.0f; return *this; }
	Value = *(float*)(_Buff + HEADER_SIZE + _front);
	_front += sizeof(Value);
	_DataSize -= sizeof(Value);
	return *this;
}

CSerialBuffer& CSerialBuffer::operator>>(int64_t& Value)
{
	if (_Sealed || IsEmpty(sizeof(Value))) { Value = 0; return *this; }
	Value = *(int64_t*)(_Buff + HEADER_SIZE + _front);
	_front += sizeof(Value);
	_DataSize -= sizeof(Value);
	return *this;
}

CSerialBuffer& CSerialBuffer::operator>>(uint64_t& Value)
{
	if (_Sealed || IsEmpty(sizeof(Value))) { Value = 0; return *this; }
	Value = *(uint64_t*)(_Buff + HEADER_SIZE + _front);
	_front += sizeof(Value);
	_DataSize -= sizeof(Value);
	return *this;
}

CSerialBuffer& CSerialBuffer::operator>>(double& Value)
{
	if (_Sealed || IsEmpty(sizeof(Value))) { Value = 0.0; return *this; }
	Value = *(double*)(_Buff + HEADER_SIZE + _front);
	_front += sizeof(Value);
	_DataSize -= sizeof(Value);
	return *this;
}

// _Sealed 체크: 정상 흐름에서는 항상 false → 분기 예측 적중으로 실질 비용 0
// Release에서도 유지하여 Seal 이후 읽기/쓰기 시도에 대한 안전성 보장
int CSerialBuffer::GetData(char* Dest, int size)
{
	if (_Sealed || IsEmpty(size))
		return 0;

	memcpy_s(Dest, size, _Buff + HEADER_SIZE + _front, size);
	_front += size;
	_DataSize -= size;
	return size;
}

int CSerialBuffer::PeekData(char* Dest, int size)
{
	if (IsEmpty(size))
		return 0;

	memcpy_s(Dest, size, _Buff + HEADER_SIZE + _front, size);
	return size;
}

// _Sealed 체크: 정상 흐름에서는 항상 false → 분기 예측 적중으로 실질 비용 0
// Release에서도 유지하여 Seal 이후 읽기/쓰기 시도에 대한 안전성 보장
int CSerialBuffer::SetData(char* Src, int size)
{
	if (_Sealed || IsFull(size))
		return 0;

	memcpy_s(_Buff + HEADER_SIZE + _rear, _BufferSize - HEADER_SIZE - _rear, Src, size);
	_rear += size;
	_DataSize += size;
	return size;
}

}   // namespace Fixed
