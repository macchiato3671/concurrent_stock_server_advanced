# ADR 003: baseline 영속화 전략으로 snapshot + append-only log를 선택

## 상태

- Status: Accepted
- Date: 2026-03-30

---

## 배경

현재 프로젝트의 baseline 목표는 안정성, 일관성, 복구 가능성을 가장 단순한 구조로 먼저 확보하는 것이다.

특히 상태 변경 요청에 대해서는 다음을 만족해야 한다.

- 성공 응답한 상태 변경은 유실되지 않아야 한다
- 서버 재시작 후에도 이전의 커밋된 상태를 복구할 수 있어야 한다
- 성공 응답은 실제 상태 변경과 영속 반영이 완료된 경우에만 반환되어야 한다

따라서 baseline 영속화 전략은 구현이 단순하면서도, 성공 응답과 복구 가능성을 명확하게 설명할 수 있어야 한다.

---

## 결정

baseline 영속화 전략으로 `snapshot + append-only log` 구조를 사용한다.

구체적으로는 다음과 같이 적용한다.

- snapshot
  - 특정 시점의 전체 stock 상태를 저장한다
- append-only log
  - snapshot 이후 발생한 성공한 상태 변경 요청을 순서대로 기록한다
- `show`는 상태를 변경하지 않으므로 로그 기록 대상이 아니다
- 성공한 `buy`, `sell`만 append-only log에 기록한다
- 성공 응답은 append-only log 기록과 durable 반영 확인 이후에만 반환한다

---

## 이유

`snapshot + append-only log`는 baseline 목표를 만족하는 가장 단순한 영속화 전략이다.

- 전체 상태와 변경 이력을 분리해서 관리할 수 있다
- 성공한 상태 변경만 기록하면 되므로 구조가 단순하다
- 성공 응답 시점을 명확하게 정의하기 쉽다
- 서버 재시작 시 `snapshot + log replay`로 복구 절차를 설명하기 쉽다
- 파일 기반 구현으로 시작할 수 있어 baseline 복잡도를 낮출 수 있다
- 이후 snapshot 정책, log compaction, SQLite 같은 대체 저장소로 확장하기 쉽다

즉, baseline 단계에서는 복잡한 저장 계층보다, 복구 가능성과 성공 응답의 의미를 명확하게 보장하는 구조가 더 중요하므로 이 전략이 적합하다.

---

## 대안

### 1. 종료 시점 단순 snapshot만 사용

구현은 가장 단순하지만, 정상 종료 이전의 성공한 상태 변경이 비정상 종료 시 유실될 수 있다.  
따라서 성공 응답한 상태 변경의 유실 방지라는 요구를 만족하기 어렵다.

### 2. 로그만 사용하고 snapshot은 사용하지 않음

구조는 가능하지만, 실행 시간이 길어질수록 replay 비용이 커지고 복구 시간이 증가한다.  
baseline에서도 복구 절차를 단순하게 유지하기 위해 snapshot을 함께 두는 편이 낫다.

### 3. SQLite 같은 DB 기반 영속화

transaction과 durability를 더 체계적으로 다룰 수 있지만, baseline 단계에서는 저장 계층 자체의 복잡도가 커질 수 있다.  
현재 단계에서는 파일 기반 구조가 더 단순하고 설명하기 쉽다.

---

## 결과

이 결정을 통해 baseline은 다음 특성을 가진다.

- 성공 응답한 상태 변경의 유실 방지 구조를 갖는다
- 재시작 후 커밋된 상태를 복구할 수 있다
- 성공 응답 시점을 append-only log durable 반영 이후로 명확히 정의할 수 있다
- 이후 snapshot 정책 개선, log compaction, SQLite 도입 같은 고도화의 기준점이 된다

---

## 한계

이 선택은 baseline에는 적합하지만 다음 한계도 가진다.

- 로그가 길어지면 replay 비용이 증가할 수 있다
- snapshot 정책이 단순하면 복구 시간이 길어질 수 있다
- atomic file replace, commit record, log compaction 같은 세부 기법은 아직 포함하지 않는다

이 한계는 baseline 이후 영속성 및 복구 고도화 단계에서 검토한다.