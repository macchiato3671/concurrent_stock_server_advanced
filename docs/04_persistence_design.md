# 04_persistence_design
## 1. 문서 목적

이 문서는 baseline 서버가 상태 변경 결과를 어떻게 영속 저장하고, 서버 재시작 시 어떤 방식으로 상태를 복구하는지를 정의한다.

baseline의 목적은 가장 단순한 구조로 다음을 만족하는 것이다.

- 성공 응답한 상태 변경은 유실되지 않아야 한다
- 서버 재시작 후에도 이전의 커밋된 상태를 복구할 수 있어야 한다

---

## 2. baseline에서 채택한 영속화 방식

baseline은 `snapshot + append-only log` 구조를 사용한다.

- snapshot
  - 특정 시점의 전체 stock 상태를 저장한 파일
- append-only log
  - snapshot 이후 발생한 성공한 상태 변경 요청을 순서대로 기록한 로그

이 구조는 전체 상태 복사본과 이후 변경 이력을 분리해서 관리할 수 있고, 복구 절차를 단순하게 설명하기 쉽다.

---

## 3. 기록 대상과 성공 응답 시점

영속 저장의 대상은 성공한 상태 변경 요청이다.

- `show`는 상태를 변경하지 않으므로 기록 대상이 아니다
- 성공한 `buy`, `sell`만 append-only log에 기록한다

상태 변경 요청은 다음 순서로 처리한다.

1. 요청 파싱 및 검증
2. 메모리 상태 변경
3. append-only log 기록
4. durable 반영 확인
5. 성공 응답 반환

즉, baseline에서는 append-only log 기록이 완료되기 전에는 성공 응답을 반환하지 않는다.

---

## 4. snapshot과 append-only log의 역할

### snapshot

snapshot은 전체 stock 상태를 저장하는 파일이다.

역할:
- 서버 시작 시 초기 상태 복원의 기준이 된다
- 전체 상태 백업 역할을 한다
- 로그 replay 범위를 줄이는 기준점이 된다

baseline에서는 우선 정상 종료 시 snapshot을 저장하는 방식을 사용한다.

### append-only log

append-only log는 성공한 상태 변경 요청만 순서대로 뒤에 추가하는 로그 파일이다.

역할:
- snapshot 이후의 상태 변경 이력을 보존한다
- 비정상 종료 이후에도 커밋된 변경을 복구할 수 있게 한다
- 성공 응답한 요청의 유실을 막는다

baseline에서는 마지막 snapshot 이후 발생한 성공한 `buy`, `sell` 요청을 append-only log에 기록한다.

---

## 5. 복구 절차

서버 재시작 시 복구는 다음 순서로 수행한다.

1. snapshot 파일을 읽는다
2. 전역 상태 저장소를 snapshot 기준으로 초기화한다
3. append-only log를 읽는다
4. snapshot 이후의 로그를 순서대로 replay한다
5. 최신 상태를 복원한다
6. 복구 완료 후 worker thread와 accept loop를 시작한다

이 절차를 통해 마지막으로 성공 응답한 상태 변경까지 복구하는 것을 목표로 한다.

---

## 6. baseline 범위와 한계

baseline에서는 영속화 구조를 단순하게 유지한다.

현재 범위에 포함하는 것:
- snapshot 기반 전체 상태 저장
- append-only log 기반 상태 변경 기록
- 재시작 시 snapshot + log replay 복구

현재 범위에 포함하지 않는 것:
- 주기적 snapshot 최적화
- log compaction
- commit record 정교화
- atomic file replace 고도화
- SQLite 같은 대체 저장소 도입

이 항목들은 baseline 이후의 고도화 대상으로 남긴다.