# ADR 002: baseline 동기화 전략으로 global mutex를 선택

## 상태

- Status: Accepted
- Date: 2026-03-30

---

## 배경

현재 프로젝트의 baseline 목표는 높은 병렬성보다 먼저 안정성, 일관성, 복구 가능성을 가장 단순한 구조로 확보하는 것이다.

이 서버는 여러 클라이언트가 동시에 `show`, `buy`, `sell` 요청을 보낼 수 있으며, 공유 상태인 stock 저장소를 안전하게 보호해야 한다.

특히 baseline에서는 다음을 우선적으로 만족해야 한다.

- 모든 stock 수량은 항상 0 이상이어야 한다
- 상태 변경 요청은 원자적으로 처리되어야 한다
- 실패한 요청은 상태를 변경해서는 안 된다
- 읽기 요청은 항상 일관된 커밋 상태만 보아야 한다
- 성공 응답은 실제 상태와 영속 반영이 완료된 경우에만 반환되어야 한다

따라서 baseline 동기화 전략은 correctness를 설명하고 검증하기 쉬운 구조여야 한다.

---

## 결정

baseline 동기화 전략으로 전역 mutex 하나를 사용한다.

구체적으로는 다음과 같이 적용한다.

- 전역 상태 저장소는 하나의 global mutex로 보호한다
- `show`, `buy`, `sell` 모두 동일한 mutex 아래에서 처리한다
- 상태 변경 요청은 검증, 메모리 변경, 로그 기록, 성공 여부 확정까지 하나의 커밋 구간으로 본다

---

## 이유

global mutex는 baseline 목표를 만족하는 가장 단순한 동기화 전략이다.

- race condition을 가장 직접적으로 제거할 수 있다
- 부분 반영 상태가 외부에 노출되는 것을 막기 쉽다
- 읽기와 쓰기 일관성을 설명하기 쉽다
- 요청 단위 상태 전이를 하나의 임계구역으로 다루기 쉽다
- 디버깅과 correctness 검증이 단순하다
- lock ordering, deadlock, finer-grained contention 같은 복잡한 문제를 뒤로 미룰 수 있다
- 영속화와 성공 응답 시점을 연결하기 쉽다

즉, baseline에서는 병렬성 확대보다 틀리지 않는 구조를 먼저 확보하는 것이 더 중요하므로 global mutex가 적합하다.

---

## 대안

### 1. Read/Write Lock

읽기 병렬성을 높일 수 있지만, baseline 단계에서는 구현과 검증이 더 복잡해진다.  
현재 단계에서는 읽기 병렬성보다 correctness 확보가 더 중요하다.

### 2. 종목별 Lock / Fine-grained Lock

충돌이 적은 요청 간 병렬성을 높일 수 있지만, lock 범위와 순서, deadlock 방지, 복합 요청 처리 등 고려사항이 크게 늘어난다.  
baseline 단계에서는 과한 선택이다.

### 3. Lock-free 또는 더 복잡한 동시성 제어

현재 프로젝트 baseline 목표와 맞지 않으며, 구현 및 검증 복잡도가 지나치게 높다.

---

## 결과

이 결정을 통해 baseline은 다음 특성을 가진다.

- 상태 전이의 correctness를 우선적으로 검증할 수 있다
- 요청 처리 흐름을 단순하게 설명할 수 있다
- 영속화와 복구 구조를 연결하기 쉽다
- 이후 read/write lock 또는 finer-grained lock과의 비교 기준점이 된다

---

## 한계

이 선택은 baseline에는 적합하지만 다음 한계도 가진다.

- `show`도 전역 mutex 아래에서 처리되므로 읽기 병렬성이 없다
- 상태 변경 요청이 직렬화되어 처리량이 제한될 수 있다
- lock contention이 커지면 병목이 발생할 수 있다

이 한계는 baseline 이후 고도화 단계에서 read/write lock, lock 세분화, contention 측정 등을 통해 검토한다.