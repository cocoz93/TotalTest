//////////////////////////////////////////////////////////////////////////
// CPP
//////////////////////////////////////////////////////////////////////////

#include "SerialBuffer.h"
#include "Platform/Platform.h"   // memcpy_s·wcslen 등 CRT 차이 흡수

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
	if (_RefCount.fetch_sub(1) == 1)
		Free(this);
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
	_Buff = new char[BufferSize];
	_BufferSize = BufferSize;

	this->Clear();
}

void CSerialBuffer::Release(void)
{
	delete[] _Buff;
}

void CSerialBuffer::Clear(void)
{
	this->_front = 0;
	this->_rear = 0;
	this->_DataSize = 0;
	this->_RefCount = 1;   // 생성자 소유권 — Alloc()이 반환하는 버퍼는 항상 RefCount=1 (사용처가 SubRef/Free로 회수)
	this->_Sealed = false;
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

	int srcTotalSize = HEADER_SIZE + SrcMsg._DataSize;
	if (srcTotalSize > _BufferSize)
		return *this;

	memcpy_s(_Buff, _BufferSize, SrcMsg._Buff, srcTotalSize);
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