

///////////////////////////////////////////////////////////////////
//CPP
///////////////////////////////////////////////////////////////////
#include "RingBuffer.h"
#include <string>

#pragma warning(push)
#pragma warning(disable:26110)
#pragma warning(disable:26451)

CRingBuffer::CRingBuffer(int bufferSize)
{
	if (bufferSize <= 0)
		bufferSize = RINGBUFF_DEFAULT_SIZE;

	_MaxSize = bufferSize;
	_Rbuff = new char[_MaxSize];
}

CRingBuffer::~CRingBuffer()
{
	if (_Rbuff != nullptr)
	{
		delete[] _Rbuff;
		_Rbuff = nullptr;
	}
}

bool CRingBuffer::IsEmpty() const
{
	return (_front == _rear);
}

bool CRingBuffer::IsFrontOverflow(int size) const
{
	return _front + size > _MaxSize;
}

bool CRingBuffer::IsRearOverflow(int size) const
{
	return _rear + size > _MaxSize;
}

int CRingBuffer::Enqueue(char* Srcbuff, int size)
{
	//버퍼에 남은 공간이 없다면 return false
	if (GetFreeSize() < size || size <= 0)
		return 0;

	if (_front <= _rear && IsRearOverflow(size) == true)
	{
		int copysize = GetDirectEnqueueSize();
		memcpy_s(_Rbuff + _rear, copysize, Srcbuff, copysize);
		memcpy_s(_Rbuff, size - copysize, Srcbuff + copysize, size - copysize);
		MoveRear(size);
		return size;
	}
	else
	{
		memcpy_s(_Rbuff + _rear, size, Srcbuff, size);
		MoveRear(size);

		return size;
	}
}

int CRingBuffer::Dequeue(char* Destbuff, int size)
{
	if (IsEmpty() || size <= 0)
		return 0;

	if (GetUseSize() < size)
		size = GetUseSize();

	if (_front > _rear && IsFrontOverflow(size))
	{
		int copysize = GetDirectDequeueSize();

		memcpy_s(Destbuff, copysize, _Rbuff + _front, copysize);
		memcpy_s(Destbuff + copysize, size - copysize, _Rbuff, size - copysize);

		//peek(Destbuff, copysize);
		//peek(Destbuff + copysize, size - copysize);

		MoveFront(size);
		return size;
	}
	else
	{
		memcpy_s(Destbuff, size, _Rbuff + _front, size);
		MoveFront(size);
		return size;
	}
}

int CRingBuffer::Peek(char* Destbuff, int size)
{
	if (IsEmpty())
		return 0;

	if (GetUseSize() < size)
		size = GetUseSize();

	if (_front > _rear && IsFrontOverflow(size))
	{
		int copysize = GetDirectDequeueSize();

		memcpy_s(Destbuff, copysize, _Rbuff + _front, copysize);
		memcpy_s(Destbuff + copysize, size - copysize, _Rbuff, size - copysize);

		//peek(Destbuff, copysize);
		//peek(Destbuff + copysize, size - copysize);

		return size;
	}
	else
	{
		memcpy_s(Destbuff, size, _Rbuff + _front, size);
		return size;
	}
}

void CRingBuffer::ClearBuffer(void)
{
	_front = 0;
	_rear = 0;
}

int CRingBuffer::MoveRear(int size)
{
	_rear = (_rear + size) % _MaxSize;
	return _rear;
}

void CRingBuffer::MoveFront(int size)
{
	_front = (_front + size) % _MaxSize;
}

int CRingBuffer::GetBufferSize(void) const
{
	return _MaxSize;
}

int CRingBuffer::GetUseSize(void) const
{
	if (_front == _rear)
		return 0;
	else if (_front < _rear)
		return _rear - _front;
	else if (_front > _rear)
		return (_MaxSize - _front) + _rear;
}

int CRingBuffer::GetFreeSize(void) const
{
	int FreeSize = _MaxSize - GetUseSize() - 1;
	if (FreeSize < 0)
		return 0;
	else
		return FreeSize;
}

int CRingBuffer::GetDirectEnqueueSize(void) const
{
	if (GetFreeSize() == 0)
		return 0;
	else if (_front <= _rear)
		return _MaxSize - _rear;
	else if (_front > _rear)
		//return _front - _rear;
		return (_MaxSize - _rear) - (_MaxSize - _front);
}

int CRingBuffer::GetDirectDequeueSize(void) const
{
	if (IsEmpty())
		return 0;
	else if (_front < _rear)
		return _rear - _front;
	else if (_front > _rear)
		return _MaxSize - _front;
}

char* CRingBuffer::GetFrontBufferPtr(void) const
{
	return _Rbuff + _front;
}

char* CRingBuffer::GetRearBufferPtr(void) const
{
	return _Rbuff + _rear;
}

char* CRingBuffer::GetRingBufferPtr(void) const
{
	return _Rbuff;
}

#pragma warning(pop)