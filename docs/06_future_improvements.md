## 1. 문서 목적

이 문서는 baseline 이후에 검토할 수 있는 고도화 방향을 정리한다.

현재 단계에서 이 모든 항목을 한 번에 구현하는 것이 목적은 아니다.  
baseline 이후 실제 병목과 목표에 따라 우선순위를 정해 순차적으로 도입하는 것을 목표로 한다.

---

## 2. 우선순위

### P1. baseline 직후 우선 검토

- 성능 측정 및 관측 가능성 강화
- 테스트 전략 고도화
- 동시성 제어 고도화
- 영속성 및 복구 고도화

### P2. 병목 확인 후 확장 검토

- 아키텍처 비교 및 확장
- 통신 신뢰성 보장 고도화
- 저장소 추상화 및 대체 영속화 계층

### P3. 완성도 향상용 확장

- 네트워크/연결 관리 고도화
- 프로토콜 설계 고도화
- 장애 대응 및 운영 안정성
- 보안 및 접근 제어
- 운영 편의성 및 관리 기능

---

## 3. P1 항목

### 3.1 성능 측정 및 관측 가능성 강화

고도화는 감이 아니라 측정 기반이어야 한다.  
따라서 baseline 이후에는 처리량, 지연시간, lock 대기 시간, recovery 시간 등을 더 체계적으로 수집하고, 병목 원인을 식별할 수 있는 구조를 강화한다.

### 3.2 테스트 전략 고도화

동시성 서버는 구현만큼 테스트가 중요하다.  
이후에는 race condition 테스트 자동화, fault injection, recovery regression test 등을 통해 숨은 동시성 오류와 복구 오류를 더 적극적으로 검증한다.

### 3.3 동시성 제어 고도화

baseline은 전역 mutex 하나로 correctness를 우선한다.  
이후에는 read/write lock 분리, lock 세분화, contention 측정을 통해 병렬성을 높일 수 있는 방향을 검토한다.

### 3.4 영속성 및 복구 고도화

baseline은 snapshot + append-only log를 사용한다.  
이후에는 snapshot 정책 개선, log compaction, recovery 검증 자동화 등을 통해 crash consistency와 recovery latency를 개선한다.

---

## 4. P2 항목

### 4.1 아키텍처 비교 및 확장

baseline 이후에는 concurrency model 자체를 비교할 수 있다.  
대표적으로 `select` 기반 버전, `epoll` 기반 버전, hybrid 구조를 구현해 correctness와 성능의 trade-off를 비교한다.

### 4.2 통신 신뢰성 보장 고도화

baseline은 TCP + Robust I/O를 사용하지만, 애플리케이션 레벨의 완전한 전달 보장은 다루지 않는다.  
이후에는 request id, 재시도 정책, 중복 요청 방지, application-level ACK 등을 통해 end-to-end 신뢰성을 강화할 수 있다.

### 4.3 저장소 추상화 및 대체 영속화 계층

baseline은 파일 기반 영속화로 시작하지만, 이후에는 storage interface 분리나 SQLite 같은 대체 저장소를 검토할 수 있다.  
이를 통해 영속화 구현을 교체 가능하게 만들고 recovery 구조를 더 단순화할 수 있다.

---

## 5. P3 항목

### 5.1 네트워크/연결 관리 고도화

connection timeout, connection limit, backpressure, keep-alive 정책 등을 통해 실제 서버 운영 품질을 높일 수 있다.

### 5.2 프로토콜 설계 고도화

명령 형식 명세화, 버전 필드 도입, 구조화된 응답 포맷 등을 통해 client/server 간 일관성을 높일 수 있다.

### 5.3 장애 대응 및 운영 안정성

graceful shutdown, crash handling, health check, fail-safe 동작 등을 통해 운영 중 안정성을 높일 수 있다.

### 5.4 보안 및 접근 제어

인증, 권한 분리, 입력 검증 강화, TLS 같은 요소는 현재 baseline 범위 밖이지만, 포트폴리오 완성도를 높이는 확장 요소가 될 수 있다.

### 5.5 운영 편의성 및 관리 기능

관리자 명령, 설정 외부화, 로그/데이터 디렉터리 구조 정리 등은 실제 프로젝트 운영성과 관리 편의성을 높이는 방향이다.

---

## 6. 현재 baseline과의 경계

현재 baseline은 다음에 집중한다.

- `pthread` 기반 thread pool
- 전역 상태 저장소
- 전역 mutex
- snapshot + append-only log
- 기본 요청/응답 프로토콜
- 기본 recovery 절차
- 기본 correctness 검증

반면 다음 항목은 baseline 이후의 고도화 대상으로 남긴다.

- request id 기반 재시도
- fine-grained locking
- read/write lock
- `select` / `epoll` 비교 구현
- SQLite 같은 저장소 교체
- 고급 관측 및 운영 기능
- fault injection 자동화
- 보안 및 인증 기능

---

## 7. 한 줄 요약

고도화는 한 번에 전부 구현하는 것이 아니라, baseline 이후 측정과 검증 결과를 바탕으로 우선순위에 따라 점진적으로 도입한다.