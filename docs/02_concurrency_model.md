## 1. 문서 목적

이 문서는 현재 프로젝트에서 고려할 수 있는 concurrency model을 간단히 비교하고 baseline 구현에 어떤 모델이 가장 적합한지 판단하기 위한 근거를 정리한다.

이 프로젝트의 목적은 단순히 많은 연결을 처리하는 것이 아니라 공유 상태를 가진 stock server에서 다음을 만족하는 구조를 설계하고 구현하는 것이다.

- 안전성
- 일관성
- 복구 가능성
- 이후 성능 비교 가능성

따라서 concurrency model 역시 단순한 연결 처리 성능만이 아니라 상태 변경의 원자성, 읽기 일관성, 영속화 연계, 구현 복잡도를 함께 기준으로 평가해야 한다.

---

## 2. 현재 프로젝트에서 현실적으로 중요한 후보

이론적으로는 다양한 concurrency model이 존재하지만 현재 C 기반 concurrent stock server 프로젝트에서 실제로 의미 있게 검토할 후보는 다음 세 가지가 중심이다.

- `pthread` 기반 Thread Pool
- `select` 기반 Event-driven
- `epoll` 기반 Event-driven

이 세 모델은 각각 다음 역할로 볼 수 있다.

- `pthread` thread pool  
  → 안정성 우선 baseline 후보
- `select`  
  → 단순 event-driven 비교 후보
- `epoll`  
  → 후속 고도화 및 확장성 비교 후보

---

## 3. 주요 후보 비교

### 3.1 `pthread` 기반 Thread Pool

#### 개념
미리 worker thread를 여러 개 생성해 두고 들어온 연결 또는 요청을 queue에 넣어 worker가 처리하는 방식이다.

#### 장점
- thread 수를 제어할 수 있어 자원 사용량을 통제하기 쉽다.
- 요청 처리 흐름을 순차 코드처럼 작성하기 쉽다.
- `buy`, `sell`, `show`를 transaction-like 하게 다루기 쉽다.
- 전역 상태 저장소 + 전역 mutex 구조와 잘 맞는다.
- snapshot + append-only log 같은 영속화 구조를 붙이기 쉽다.
- correctness와 구현 단순성의 균형이 좋다.

#### 단점
- queue 설계가 필요하다.
- worker wakeup, shutdown, queue full 처리 같은 세부 설계가 필요하다.
- 전역 lock을 크게 잡으면 contention이 커질 수 있다.
- event-driven 구조보다 대규모 연결 처리 효율은 떨어질 수 있다.

#### 현재 프로젝트 적합도
현재 프로젝트의 baseline 목표는 최고 성능보다 안정성, 일관성, 복구 가능성을 먼저 확보하는 것이다.  
이 기준에서는 `pthread` 기반 thread pool이 가장 현실적이고 균형 잡힌 선택이다.

---

### 3.2 `select` 기반 Event-driven

#### 개념
하나의 스레드 또는 소수의 스레드가 `select()`로 여러 fd를 감시하고 준비된 fd를 순차적으로 처리하는 방식이다.

#### 장점
- single-thread event loop라면 shared state race condition 설명이 단순해진다.
- thread synchronization 문제가 줄어든다.

#### 단점
- `FD_SETSIZE` 제한이 있다.
- 감시 fd 수가 많아질수록 비효율적이다.
- 매번 fd set 전체를 순회해야 한다.
- blocking 작업 하나가 전체 이벤트 루프를 막을 수 있다.
- per-client buffer 관리 등 I/O 상태 관리가 번거롭다.

#### 현재 프로젝트 적합도
`select`는 event-driven 모델과 thread 기반 모델의 차이를 보여주는 비교 대상으로는 의미가 있다.  
다만 현재 프로젝트의 baseline 1순위로 보기에는 확장성과 구현 편의성 측면에서 다소 아쉬움이 있다.

---

### 3.3 `epoll` 기반 Event-driven

#### 개념
Linux에서 많은 fd를 효율적으로 감시하기 위한 고성능 이벤트 통지 메커니즘이다.

#### 장점
- 많은 연결 처리에 유리하다.
- `select`보다 확장성이 좋다.
- ready된 fd만 처리하므로 효율이 높다.
- Linux 고성능 서버 구조를 실험하고 비교하기에 좋다.
- 성능 고도화 단계에서 의미가 크다.

#### 단점
- non-blocking I/O 이해가 필요하다.
- partial read/write를 직접 다뤄야 한다.
- per-connection state 관리가 필요하다.
- edge-triggered와 level-triggered 차이를 이해해야 한다.
- 구현 실수 시 디버깅이 어렵다.
- baseline 단계에서는 복잡도가 크다.

#### 현재 프로젝트 적합도
`epoll`은 성능 및 확장성 실험을 위한 후속 고도화 후보로는 매우 적합하다.  
하지만 현재 baseline 단계에서는 correctness와 durability 검증보다 I/O 메커니즘 구현 자체가 더 큰 비중을 차지할 수 있어 초기 후보로는 부담이 있다.

---

## 4. 후순위 또는 범위 외 후보

현재 프로젝트와 직접적인 관련성은 상대적으로 낮지만 이론적으로는 다음과 같은 모델도 존재한다.

### 4.1 순차 처리형
- Single-threaded Sequential Server

가장 단순하고 correctness 검증은 쉽지만 concurrent server라는 현재 프로젝트 목표와는 맞지 않는다. 프로토타입 검증용 정도로만 의미가 있다.

### 4.2 연결당 실행 흐름 생성 방식
- Process-per-Connection
- Thread-per-Connection

직관적이지만 자원 사용량 통제가 어렵고 대규모 연결 처리나 공유 상태 관리 측면에서 한계가 있다.  
학습용 또는 초기 실험용으로는 가능하지만 현재 baseline 1순위는 아니다.

### 4.3 비동기 I/O / completion 기반 모델
- POSIX AIO
- Windows IOCP
- Linux `io_uring`

이들은 높은 성능과 확장성 측면에서는 의미가 있지만 현재 프로젝트의 baseline 목표와는 다소 거리가 있다.

현재 프로젝트의 우선순위는 다음과 같다.

- 안정성 우선
- correctness / durability 우선
- 구현 복잡도 통제
- 비교 가능한 baseline 확보

이 기준에서 비동기 I/O 계열은 학습 난도와 구현 복잡도가 높고 프로젝트 중심이 동시성 제어보다 최신 I/O 메커니즘 실험으로 이동할 가능성이 있다.  
따라서 이번 프로젝트에서는 baseline 후보로 깊게 다루지 않고 고급 확장 주제로만 간단히 언급한다.

### 4.4 Hybrid / 기타 모델
- Event Loop + Worker Thread Pool
- Reactor / Proactor
- User-level Thread / Coroutine 기반 모델
- Actor-like Model

이들은 개념적으로는 흥미롭고 확장 주제로도 의미가 있지만 현재 baseline 단계에서는 복잡도가 높고 프로젝트 범위를 지나치게 키울 가능성이 있다.  
따라서 우선순위를 낮게 둔다.

---

## 5. 현재 프로젝트 기준 평가

현재 프로젝트의 우선순위는 다음과 같다.

- 안정성 우선
- correctness / durability 우선
- 구현 단순성 확보
- 이후 확장 가능성 고려

이 기준으로 보면 각 핵심 후보는 다음처럼 정리할 수 있다.

- `pthread` thread pool  
  - 요청 단위 상태 변경을 다루기 쉽다.
  - 성공 응답 전 영속 반영 구조를 붙이기 쉽다.
  - 읽기/쓰기 일관성을 설명하기 쉽다.
  - baseline 구현에 가장 적합하다.

- `select`  
  - event-driven 비교 대상으로 의미가 있다.
  - race condition 설명은 단순할 수 있다.
  - 그러나 확장성과 I/O 관리 측면에서 baseline 1순위는 아니다.

- `epoll`  
  - Linux 고성능 서버 구조로서 의미가 크다.
  - 고도화 실험에는 적합하다.
  - 하지만 baseline 단계에서는 구현 복잡도가 높다.

---

## 6. 결론

현재 프로젝트의 baseline에서 최우선 목표는 안정성이다.

먼저 공유 상태를 가진 stock server가 일관된 상태 전이, 성공 응답의 신뢰성, 재시작 이후 복구 가능성을 만족하도록 만들어야 한다.  
다만 이 프로젝트는 concurrent server이므로, 순차 처리에 머무르지 않고 최소한의 병렬성 또한 만족해야 한다.

따라서 baseline concurrency model은 안정성을 우선적으로 확보하면서도 동시 요청을 처리할 수 있는 가장 단순한 구조여야 한다.

이 기준에서 현재 프로젝트의 baseline 후보로 가장 적합한 모델은 `pthread` 기반 thread pool이다.

따라서 현재 프로젝트는 다음과 같은 흐름으로 진행한다.

1. baseline은 `pthread` 기반 thread pool로 구현한다.
2. 상태 계층은 전역 상태 저장소와 단순한 동기화 전략으로 시작한다.
3. 이후 비교 대상으로 `select`를 검토할 수 있다.
4. 후속 고도화 단계에서 `epoll` 기반 구조를 검토한다.