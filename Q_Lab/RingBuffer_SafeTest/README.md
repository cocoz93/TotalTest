
링버퍼테스트.


flowchart LR
    subgraph Phase1[Phase 1: 단일 스레드]
        direction TB
        D1[데이터 무결성<br/>시퀀스 검증]
        D2[불변성 검증<br/>DataSize/FreeSize]
        D3[경계 조건<br/>Wrap-Around]
    end
    
    subgraph Phase2[Phase 2: 멀티스레드]
        direction TB
        M1[Producer-Consumer<br/>8가지 조합]
        M2[Peek-Consume<br/>8가지 조합]
        M3[고빈도 경합<br/>5가지 스레드 수]
    end
    
    subgraph Validation[검증 항목]
        direction TB
        V1[데이터 순서]
        V2[중복/누락]
        V3[불변 조건]
        V4[경계 안전성]
        V5[동시성 안전성]
        V6[처리량]
    end
    
    D1 --> V1
    D1 --> V2
    D2 --> V3
    D3 --> V4
    
    M1 --> V2
    M1 --> V5
    M2 --> V5
    M2 --> V2
    M3 --> V5
    M3 --> V6
    
    V1 --> Result[✓ 100% PASS]
    V2 --> Result
    V3 --> Result
    V4 --> Result
    V5 --> Result
    V6 --> Result
    
    style Phase1 fill:#fff4e6
    style Phase2 fill:#e8f5e9
    style Validation fill:#f3e5f5
    style Result fill:#c8e6c9