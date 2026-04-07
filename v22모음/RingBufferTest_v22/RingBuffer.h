
#pragma once

#ifndef ____RING_BUFFER____
#define ____RING_BUFFER____

const int RINGBUFF_DEFAULT_SIZE = 4096;

class CRingBuffer
{
	friend class CNetServer;

public:
	explicit CRingBuffer(int bufferSize = RINGBUFF_DEFAULT_SIZE);
	virtual ~CRingBuffer();

public:
	bool IsEmpty() const;
	//bool IsFull(int size);

public:
	int Enqueue(char* Srcbuff, int size);
	int Dequeue(char* Destbuff, int size);
	int Peek(char* Destbuff, int size);
	void ClearBuffer(void);
	
	//void Resize(int size);
	int GetBufferSize(void) const;
	int GetUseSize(void) const;	//사용 버퍼 사이즈
	int GetFreeSize(void) const;	//남은 버퍼 사이즈

private:
	// 내부에서 사용하는 front/rear 체크용 함수
	bool IsFrontOverflow(int size) const; //
	bool IsRearOverflow(int size) const;
	int MoveRear(int size);
	void MoveFront(int size);

	int GetDirectEnqueueSize(void) const;
	int GetDirectDequeueSize(void) const;

	char* GetFrontBufferPtr(void) const;
	char* GetRearBufferPtr(void) const;
	char* GetRingBufferPtr(void) const;

private:
	int _front = 0;
	int _rear = 0;
	int _MaxSize = 0;		//실제 링버퍼사이즈. MAXSIZE == 100일경우 Data가들어갈자리는 99까지.
	char* _Rbuff = nullptr;
};

#endif //____CRingBuffer_____
