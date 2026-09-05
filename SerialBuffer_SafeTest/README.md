```
[메뉴 구조]
═══════════════════════════════════
  1 ─ Phase 1-1: 전 타입 왕복
  2 ─ Phase 1-2: 랜덤 혼합 타입 왕복
  3 ─ Phase 1-3: 문자열 직렬화
  4 ─ Phase 1-4: 경계 조건
  5 ─ Phase 1 전체 (1→2→3→4)
  6 ─ Phase 2-1: Seal 봉인
  7 ─ Phase 2-2: 참조 카운팅
  8 ─ Phase 2 전체 (6→7)
  9 ─ Phase 3-1: 브로드캐스트 시나리오
 10 ─ Phase 3-2: 다중 스레드 수명 스트레스
 11 ─ Phase 3-3: 전송 실패 경로
 12 ─ Phase 3-4: 타겟 0명 브로드캐스트
 13 ─ Phase 3-5: 단건+배치 AddRef 혼합
 14 ─ Phase 3 전체 (9→…→13)
 15 ─ Phase 4: 잠재 결함 점검 (비파괴)
 16 ─ 전체 실행 (1→…→15)
  0 ─ 종료
═══════════════════════════════════

SerialBuffer_SafeTest/
├── SerialBuffer_SafeTest.sln
├── SerialBuffer_SafeTest.vcxproj
├── SerialBuffer_SafeTest.cpp     ← 테스트 코드
├── SerialBuffer.h                ← 테스트 대상 (원본 사본, 수정 금지)
├── SerialBuffer.cpp              ← 테스트 대상 (원본 사본, 수정 금지)
├── LockFreeConfig.h              ← 테스트용 대체 헤더
└── Platform/Platform.h           ← 테스트용 대체 헤더

[원본]
  MMO/MMOServer/MMOServer/SerialBuffer.{h,cpp} 를 바이트 단위로 그대로 복사한 것.
  원본을 고치면 이 사본도 같이 갱신해야 한다. (RingBuffer.h 가 이미 갈라진 전례 있음)

[대체 헤더를 둔 이유]
  원본은 형제 저장소(cocoz93/LockFree)와 470줄짜리 Platform.h 에 의존한다.
  이 프로젝트의 대상은 CSerialBuffer 하나뿐이고 락프리 자료구조는 별도 테스트에서
  다루므로, 의존 지점만 최소한으로 대체해 단독 빌드가 되게 했다.

  · LockFreeConfig.h  — CExternalTlsFreeList 를 new/delete 기반으로 대체.
                        부수 효과로 Alloc/Free 호출 횟수가 계측되어,
                        참조카운트가 Free 를 정확히 1회 부르는지 검증할 수 있다.
  · Platform/Platform.h — SerialBuffer.cpp 가 쓰는 memcpy_s·wcslen·strlen 만 제공.
                        MSVC 에서는 사실상 <string.h> 포함일 뿐이다.

  → 실제 TLS 락프리 프리리스트를 태운 검증은 LockFree 테스트에서 따로 한다.

[버퍼 구조]
  [ 헤더 2B ][ 페이로드 ................................ ]
  0    HEADER_SIZE                              _BufferSize
             ↑ _front ≤ _rear,  _DataSize = _rear - _front

  기본 크기 MSG_DEFAULT_SIZE = 1460 (현재 프로토콜 최대 패킷 ~1034B 수용)

[수명 모델]
  Alloc() → operator<< → Seal() → AddRef(N) → WSASend × N → SubRef()
  · Alloc 이 반환하는 버퍼는 RefCount=1 (생성자 소유권)
  · AddRef(N) 은 브로드캐스트 타겟 수만큼 원자연산 1회로 압축
  · SubRef 가 0으로 떨어뜨린 스레드가 Free 를 호출 — 정확히 1회여야 한다

[Phase 1: 직렬화 기본] ── 단일 스레드
│
├── 1-1. 전 타입 왕복 (300만회)
│     uint8/char/short/uint16/int/uint32/int64/uint64/float/double 순서·값 보존
├── 1-2. 랜덤 혼합 타입 왕복 (300만 라운드)
│     라운드마다 타입·개수를 무작위로 섞어 순서 보존과 값 손상 검증
├── 1-3. 문자열 직렬화 (100만회)
│     [short 길이][본문] 형식. wide 는 길이가 바이트 수임을 확인.
│     대응 operator>> 가 없어 수신측은 길이를 읽고 GetData 로 본문을 가져간다
└── 1-4. 경계 조건
      IsFull/IsEmpty, 정확히 가득 채우기, 초과 쓰기 거부,
      언더플로우 시 값 0 초기화·위치 불변, PeekData 비파괴,
      MoveWritePos/MoveReadPos(초과 시 보유량까지만 클램프),
      헤더 2바이트 예약 영역과 페이로드의 분리

[Phase 2: 수명 / 봉인]
│
├── 2-1. Seal 봉인
│     봉인 후 operator<< / SetData / operator>> / GetData 는 모두 차단,
│     PeekData 만 열려 있음(브로드캐스트 읽기 경로), Clear 로 봉인 해제
└── 2-2. 참조 카운팅 (100만 사이클)
      Alloc 직후 RefCount=1, AddRef/SubRef 균형,
      배치 AddRef(N) + SubRef×N + 소유권 반납 → Free 정확히 1회,
      대량 사이클 후 누수 0 (프리리스트 카운터로 확인)

[Phase 3: 멀티스레드 / 실사용 소유권 경로]
│
├── 3-1. 브로드캐스트 시나리오 (20만 라운드 × 2·4·8·16 타겟)
│     송신: Alloc→직렬화→Seal→AddRef(N)→N개 워커 큐 투입→SubRef
│     워커: 큐에서 꺼내 PeekData 로 읽고 SubRef
│     마지막 SubRef 가 어느 스레드에서 날지 모르는 경합에서
│     Free 가 라운드 수만큼만 일어나는지가 핵심
├── 3-2. 다중 스레드 수명 스트레스 (2·4·8·16 스레드 × 30만 사이클)
│     스레드별 독립 버퍼 왕복, 교차 오염·누수 검증
├── 3-3. 전송 실패 경로 (20만 라운드)
│     RequestSendMsg 의 계약 — "성공이든 실패든 ref 를 정확히 1개 소비" —
│     을 모사한다. 세션 무효 / ABA 검출 / SendQ 상한을 섞어 돌리고,
│     전 타겟 실패 케이스와 MT(워커 4·16) 케이스까지 본다.
│     맨 앞에 "검증 장치 자체 확인"이 있다 — 일부러 소비를 1회 빠뜨려
│     누수가 실제로 검출되는지 먼저 보이고 시작한다.
├── 3-4. 타겟 0명 브로드캐스트 (50만 라운드)
│     validCount==0 이라 배치 AddRef 를 생략하는 실제 경로,
│     AddRef(0) 을 명시 호출하는 변형, 0명/N명 혼합
└── 3-5. 단건+배치 AddRef 혼합 (20만 라운드)
      한 버퍼에 AddRef() 와 AddRef(N) 을 섞어 쓰는 실제 패턴
      (직송 등록 + 섹터 팬아웃 + 점프 폴백).
      부여와 소비가 서로 다른 스레드에서 일어나는 MT 케이스 포함

[Phase 4: 잠재 결함 점검 (비파괴)]
  코드를 읽어 찾은 의심 지점이 실제로 성립하는지 계산으로만 확인하고
  경고를 남긴다. 실제로 트리거하면 버퍼 밖에 쓰거나 쓰레기를 읽으므로
  죽이지 않는다. 현재 6건 검출:

  1) Clear 없이 전부 읽은 뒤 다시 쓰기
     IsFull 은 _DataSize 기준인데 쓰기는 _rear 위치에 한다.
     읽기로 _DataSize 만 줄면 "여유 있음" 판정과 실제 남은 공간이 어긋난다.
     → 사용 계약: 재사용 전 반드시 Clear() 또는 Alloc()

  2) 부분 읽기 상태에서의 operator= 복사
     복사 길이가 HEADER_SIZE + _DataSize 인데 유효 구간 끝은
     HEADER_SIZE + _rear 다. _front > 0 이면 뒷부분이 빠진다.
     → 사용 계약: 읽기 전(_front==0) 상태에서만 복사 대입

  3) 소유권 초과 반납
     SubRef 는 fetch_sub 결과가 정확히 1일 때만 Free 를 부른다.
     AddRef 보다 SubRef 를 많이 부르면 RefCount 가 음수로 내려가고
     회수는 영영 일어나지 않는다 (조용한 누수).
     → 사용 계약: AddRef 횟수와 SubRef 횟수를 정확히 맞출 것

  4) 버퍼 수명 관리가 생성자/소멸자 밖으로 샘
     · Release() 는 _Buff 를 null 로 만들지 않는데 public 이고
       소멸자도 Release() 를 부른다 → 명시 호출 후 소멸 시 이중 해제
     · Initialize() 도 이전 _Buff 를 해제하지 않고 덮어쓴다 → 두 번 부르면 누수
     → 둘 다 현재 외부 호출처 없음(잠재).
       계약: Release()/Initialize() 를 직접 부르지 말 것

     ※ Initialize 쪽은 실행 점검을 하지 않는다. "이전 버퍼가 해제됐는지"를
       UB 없이 관찰할 수 없고, 테스트가 대신 delete[] 해 주면 원본이 고쳐졌을 때
       그 해제가 이중 해제가 되어 테스트가 수정을 방해한다. 초기 버전이 실제로
       그렇게 짜여 있었고 역방향 뮤테이션에서 걸렸다.

     ※ 고칠 때 주의: Initialize 에 delete[] 만 추가하는 부분 수정은 위험하다.
       Release 의 null 처리가 선행되지 않으면 이중 해제가 된다 (검증됨).

  5) 문자열 길이 오버플로우
     short Len = (short)strlen(Value) 이라 32767 초과 시 음수가 되고,
     그 음수가 IsFull 계산에 그대로 들어가 가드를 통과한다.
     이어지는 SetData 가 음수 크기로 _rear 를 뒤로 민다.
     → 기본 버퍼(1460)에서는 도달 불가. 32KB 이상 버퍼에서만 위험

  6) operator= 크기 초과 시 조용한 실패
     대상 버퍼가 작으면 복사를 건너뛰지만 반환값이 없어 호출부가 모른다.
     복사됐다고 믿고 쓰면 빈 버퍼를 전송하게 된다.
     → 계약: 복사 대입 전 대상 버퍼 크기를 직접 확인할 것

[반복 횟수 조절]
  SerialBuffer_SafeTest.cpp 상단 namespace TestConfig 의 상수를 수정한다.
```
