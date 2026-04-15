# ADR 001: baseline concurrency model로 pthread 기반 thread pool을 선택
## 상태

- Status: Accepted
- Date: 2026-03-30

---

## 배경

현재 프로젝트의 baseline 목표는 최고 성능보다 안정성, 일관성, 복구 가능성을 가장 단순한 구조로 먼저 확보하는 것이다.

동시에 이 프로젝트는 concurrent stock server이므로, 순차 처리 구조에 머무르지 않고 여러 클라이언트의 동시 요청을 처리할 수 있어야 한다.

따라서 baseline concurrency model은 다음 조건을 만족해야 한다.

- 안정성을 우선적으로 확보할 수 있는가
- concurrent server로서 필요한 최소한의 병렬성을 만족하는가
- 요청 단위 상태 변경을 다루기 쉬운가
- 구현과 검증이 단순한가
- 이후 성능 비교와 구조 확장이 가능한가

---

## 결정

baseline concurrency model로 `pthread` 기반 thread pool을 사용한다.

구체적으로는 다음 구조를 사용한다.

- Main Thread가 클라이언트 연결을 accept한다
- accept된 connection(`connfd`)은 bounded queue에 넣는다
- 여러 worker thread가 queue에서 connection을 가져와 요청을 반복 처리한다

---

## 이유

`pthread` 기반 thread pool은 현재 baseline 목표에 가장 잘 맞는 가장 단순한 concurrent 구조다.

- 순차 처리 구조보다 병렬성을 확보할 수 있다
- thread-per-connection보다 thread 수를 통제하기 쉽다
- 요청 처리 흐름을 순차 코드처럼 작성하기 쉽다
- `buy`, `sell`, `show`를 connection 안의 request 단위로 다루기 쉽다
- 전역 상태 저장소 + 전역 mutex 구조와 잘 맞는다
- snapshot + append-only log 기반 영속화와 복구 구조를 붙이기 쉽다
- correctness 검증과 디버깅이 비교적 단순하다
- 이후 `select`, `epoll` 기반 구조와 비교하기 위한 기준점이 된다

---

## 대안

### 1. Single-threaded Sequential Server

가장 단순하고 correctness 설명은 쉽지만, concurrent server라는 현재 프로젝트 목표를 만족하지 못한다.

### 2. `select` 기반 event-driven server

비교용 모델로는 의미가 있지만, baseline 단계에서는 I/O multiplexing과 상태 관리가 구현 복잡도를 높일 수 있다.

### 3. `epoll` 기반 event-driven server

고도화 및 확장성 비교에는 의미가 크지만, baseline 단계에서는 non-blocking I/O와 per-connection state 관리 등으로 인해 복잡도가 높다.

### 4. Thread-per-Connection

구현은 직관적이지만, 연결 수 증가에 따라 thread 수가 늘어나 자원 통제가 어렵다.

---

## 결과

이 결정을 통해 baseline은 다음 특성을 가진다.

- 안정성을 우선하는 단순한 concurrent 구조를 갖는다
- 전역 상태 저장소와 단순한 lock 전략을 적용하기 쉽다
- 영속화 및 recovery 구조를 연결하기 쉽다
- 이후 고도화 버전과 비교 가능한 기준 구현이 된다

---

## 한계

이 선택은 baseline에는 적합하지만 다음 한계도 가진다.

- 전역 lock 기반 구조에서는 병렬성이 제한될 수 있다
- 많은 연결을 다루는 상황에서는 event-driven 구조보다 비효율적일 수 있다
- lock contention이 커지면 처리량이 제한될 수 있다

이 한계는 baseline 이후 고도화 단계에서 검토한다.