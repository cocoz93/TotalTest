# TotalTest

여러가지 테스트를 진행하는 리포지토리.
성능, 안전성, 라이브러리간 비교 등 여러 실험을 모아둔 곳입니다.

대상은 [ServerCore](https://github.com/cocoz93/ServerCore) 의 [정본](#정본을-시험합니다)이고, 지금까지 [결함 하나](#시험이-잡은-것)를 잡았습니다.

## 시험

각 줄을 펼치면 무엇을 어디까지 보는지 나옵니다.

<details>
<summary><b>링버퍼 <code>CRingBuffer</code></b> — 정합성 시험 아홉 · 1,344줄</summary>

`RingBuffer_SafeTest/` · 메뉴에서 번호를 골라 하나씩 돌립니다.

**Phase 1 · 단일 스레드**

| | 보는 것 | 규모 |
|---|---|---|
| **1-1 데이터 무결성** | 번호를 매겨 넣고 빼며 순서가 어긋나거나 깨지지 않나 | 1억 회 |
| **1-2 불변성** | 넣기·빼기·훔쳐보기·버리기를 섞어 돌려도 `DataSize + FreeSize == capacity - 1` 인가 | 1억 회 |
| **1-3 경계 조건** | 버퍼 끝에서 되감기 · 여러 크기로 순환 · 용량 초과 막기 | 2,500만 × 3 |
| **1-4 잘못된 사용** | `nullptr` · 크기 0 · capacity 0 · 용량 초과를 **거절하는지** | 검사 7가지 |

**Phase 2 · 멀티스레드**

| | 보는 것 | 규모 |
|---|---|---|
| **2-1 생산자–소비자** | 넣은 숫자가 빠짐도 겹침도 없이 그대로 나오나 · 부분 읽기가 생기지 않나 | 조합 8가지(대칭 4 · 비대칭 4) · 생산자당 1,000만 |
| **2-2 고빈도 경합** | 16B 패킷(매직 `0xDEADBEEF` + 체크섬)이 왕복 뒤에도 그대로인가 | 스레드 2·4·8·16·32 |

**Phase 3 · Zero-copy**

복사 없이 버퍼를 직접 가리켜 주고받는 경로 — `WSARecv` · `WSASend` 가 쓰는 길입니다.

| | 보는 것 | 규모 |
|---|---|---|
| **3-1 직접접근 왕복** | `GetWritePtr` → `MoveWritePtr` → `GetReadPtr` → `Consume` | 2,500만 회 |
| **3-2 경계 랩 집중** | 256B 버퍼로 직접쓰기를 매번 두 조각으로 쪼개 포인터 계산을 정조준 | 2,500만 회 |
| **3-3 `GetSendInfo`** | 읽는 **동안** 생산자가 계속 넣어도 스냅샷 구간이 그대로인가 | 1억 바이트 · 생산자 1 + 소비자 1 |

</details>

<details>
<summary><b>메모리풀 <code>CMemoryPool</code></b> — 성능 비교 · <code>v25/MemoryPool_v25</code></summary>

`TlsAlloc` 으로 스레드마다 캐시(64~512칸)를 두고 바탕은 `VirtualAlloc` 으로 잡는 고정 크기 블록 풀.
`new`/`delete` 와 여덟 갈래로 견줍니다 — 단일 스레드(기본 · RAII 래퍼 · 할당과 해제를 섞은 것), 여러 스레드(스레드 수를 올려 가며 · 같은 풀에 몰아넣는 경합).
객체는 64B · 256B 둘.

여기서 끝난 실험입니다.

</details>

<details>
<summary><b>TLS 프로파일러</b> — 도구 시제품 · <code>v25/TlsProfiler_v25</code></summary>

`PROFILE_SCOPE("이름")` 한 줄로 구간 시간을 재는 프로파일러.
**재는 동안에는 잠그지 않습니다** — 스레드마다 제 저장소(256칸)에 쌓아 두었다가 보고서를 뽑을 때만 합칩니다.
구간마다 총 시간 · 호출 수 · 최소 · 최대를 냅니다.

여기서 끝난 실험입니다.

</details>

## 정본을 시험합니다

ServerCore 를 서브모듈로 걸어 `..\ServerCore\Base\RingBuffer.h` 를 그대로 빌드에 넣습니다.
헤더를 치우면 `C1083` 으로 멈추니, 시험이 그 파일을 쓴다는 것이 빌드로 증명됩니다.
전에 두던 사본은 정본과 갈라져 있어서 버렸습니다(2026-09-17).

## 시험이 잡은 것

**`InitExternal` 재호출** — `Init` 은 두 번 불러도 되는데 형제 함수에는 같은 방어가 없었습니다.
`Init` 뒤에 부르면 버퍼가 새고 읽기·쓰기 위치가 그대로 남아, **새 버퍼의 엉뚱한 자리를 읽는 소리 없는 손상**이 됩니다.

고친 뒤가 아니라 **고치기 전 헤더로 같은 시험을 돌려** 정말 걸리는지부터 봤습니다.

```
LeakSanitizer: 1024 byte(s) leaked
4 FAIL — 위치 리셋 / FreeSize / 재InitExternal 위치 / 내용 불일치
```

## 아직 없는 것

- 링버퍼 **제출 추적 층**(`MarkSubmitted` 계열) 시험
- 링버퍼 Phase 1~3 완주 기록 — 부분만 돌려 봤습니다
- 자동화 — 셋 다 대화형이라 CI 에 못 붙입니다

## 빌드·실행

```
git clone --recursive https://github.com/cocoz93/TotalTest.git
git submodule update --init          # 이미 받았다면 이것만
```

서브모듈을 안 받으면 링버퍼 시험이 `RingBuffer.h` 를 못 찾고 멈춥니다.
솔루션은 시험마다 따로입니다 — Visual Studio 2022(v143) · **x64 Release**.

| 시험 | 솔루션 |
|---|---|
| 링버퍼 | `RingBuffer_SafeTest/RingBuffer_SafeTest.sln` |
| 메모리풀 | `v25/MemoryPool_v25/MemoryPool_v25.sln` |
| TLS 프로파일러 | `v25/TlsProfiler_v25/TlsProfiler_v25.sln` |

링버퍼 Phase 1·2 는 억 단위라 Debug 로는 한참 걸립니다.

## 함께 보는 저장소

| 저장소 | 내용 |
|--------|------|
| [ServerCore](https://github.com/cocoz93/ServerCore) | 여기서 시험하는 정본 — C++17 서버 토대 |
| [MMO_Zone](https://github.com/cocoz93/MMO_Zone) | 이 링버퍼를 실제로 쓰는 Windows IOCP MMO 서버 |
| [LockFree](https://github.com/cocoz93/LockFree) | 락프리 큐·스택 + 2계층 메모리풀 |

코딩 규약은 [CONTRIBUTING.md](CONTRIBUTING.md) 에 있습니다.
