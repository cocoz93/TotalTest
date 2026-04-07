```
[메뉴 구조]
═══════════════════════════════════
  1 ─ Phase 1-1: 데이터 무결성
  2 ─ Phase 1-2: 불변성 검증
  3 ─ Phase 1-3: 경계 조건
  4 ─ Phase 1-4: 잘못된 사용 방어
  5 ─ Phase 1 전체 (1→2→3→4)
  6 ─ Phase 2-1: Producer-Consumer
  7 ─ Phase 2-2: 고빈도 경합
  8 ─ Phase 2 전체 (6→7)
  9 ─ 전체 실행 (1→2→3→4→6→7)
  0 ─ 종료
═══════════════════════════════════

RingBuffer_SafeTest/
├── RingBuffer_SafeTest.sln
├── RingBuffer_SafeTest.vcxproj
├── RingBuffer_SafeTest.cpp          ← 테스트 코드
└── ../RingBuffer.h                  ← 테스트 대상

[Phase 1: 싱글스레드] ── CRingBufferST
│
├── 1-1. 데이터 무결성 (1억회)
│     시퀀스 번호 Enqueue/Dequeue 반복, 순서·손상 검증
├── 1-2. 불변성 검증 (1억회)
│     랜덤 작업 후 DataSize + FreeSize == capacity-1 검증
├── 1-3. 경계 조건 (7500만회)
│     Wrap-Around / 다양한 크기 순환 / 오버플로우 방어
└── 1-4. 잘못된 사용 방어
      nullptr, size=0, capacity=0, 오버플로우 방어 확인

[Phase 2: 멀티스레드] ── CRingBufferMT
│
├── 2-1. Producer-Consumer (8가지 조합)
│     다중 P/C 스레드, 숫자 누락·중복 없이 전수 검증
└── 2-2. 고빈도 경합 (5가지 스레드수)
      16B 패킷(magic+checksum) Enqueue/Dequeue, 무결성 검증
```