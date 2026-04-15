# 03_baseline_design
## 1. baseline 목표

이 baseline의 목표는 안정성, 일관성, 복구 가능성을 가장 단순한 구조로 먼저 확보하는 것이다.

구체적으로는 다음을 만족하는 것을 목표로 한다.

- 성공 응답한 상태 변경은 정확하게 반영되어야 한다
- 성공 응답한 상태 변경은 유실되지 않아야 한다
- 읽기 요청은 항상 일관된 상태만 보아야 한다
- 서버 재시작 후에도 상태를 복구할 수 있어야 한다

즉, baseline은 최고 성능보다 먼저 틀리지 않는 서버를 만드는 데 목적이 있다.

---

## 2. baseline 구조

baseline은 다음 구조를 사용한다.

- Main Thread
  - 클라이언트 연결 accept 담당
- Worker Thread Pool
  - queue에서 connection을 꺼내 client 요청 처리 담당
- Bounded Queue
  - accept된 `connfd`를 worker에게 전달
- Global Stock Store
  - 현재 stock 상태를 저장하는 전역 상태 저장소
- Global Mutex
  - 상태 변경과 읽기 일관성 보호
- Snapshot File
  - 전체 상태 저장
- Append-only Log
  - 성공한 상태 변경 요청 기록

이 구조는 안정성을 우선하면서도 concurrent server로서 필요한 최소한의 병렬성을 만족하는 가장 단순한 형태를 목표로 한다.

---

## 3. 상태 저장소와 동기화

전역 상태 저장소는 현재 stock 상태의 단일 진실 원천이다.  
모든 `show`, `buy`, `sell` 요청은 이 저장소를 기준으로 처리하며 상태 변경도 이곳에서만 발생한다.

동기화는 baseline 단계에서 전역 mutex 하나로 처리한다.

- 읽기와 쓰기 모두 같은 mutex 아래에서 처리한다
- 부분 반영 상태가 외부에 노출되지 않도록 한다
- 성공 응답 전 필요한 핵심 커밋 구간을 하나의 임계구역으로 본다

이 단계에서는 병렬성 확대보다 race condition 제거, 상태 전이 검증, 디버깅 용이성이 더 중요하다.

---

## 4. 요청 처리 방식

baseline은 다음 명령을 지원한다.

- `show`
- `buy <stock_id> <quantity>`
- `sell <stock_id> <quantity>`

공통 처리 흐름은 다음과 같다.

1. 요청 수신
2. 파싱
3. 입력 검증
4. 필요한 경우 mutex 획득
5. 상태 조회 또는 상태 변경
6. 응답 생성
7. mutex 해제
8. 응답 전송

`show`는 항상 일관된 커밋 상태만 조회해야 하며
`buy`와 `sell`은 검증, 상태 변경, 로그 기록, 성공 여부 확정이 하나의 커밋 단위처럼 처리되어야 한다.

성공 응답은 상태 변경과 영속 기록이 완료된 경우에만 반환한다.

---

## 5. 영속성 및 복구

baseline은 snapshot + append-only log 구조를 사용한다.

- Snapshot
  - 전체 stock 상태를 저장한다
  - 서버 시작 시 초기 복원의 기준이 된다

- Append-only Log
  - 성공한 상태 변경 요청만 순서대로 기록한다
  - 성공 응답을 보내기 전에 기록이 완료되어야 한다

서버 재시작 시에는 다음 절차로 복구한다.

1. snapshot 로드
2. 전역 상태 저장소 초기화
3. append-only log replay
4. 최신 상태 복원
5. listen socket 준비 후 worker thread와 accept loop 시작

이 구조를 통해 마지막으로 성공 처리된 상태 변경까지 복구 가능하도록 한다.

---

## 6. 실패 처리 원칙

다음과 같은 경우 요청은 실패로 처리한다.

- 잘못된 형식의 요청
- 존재하지 않는 stock
- 수량 부족
- 로그 기록 실패

실패한 요청은 상태를 변경해서는 안 되며 이미 메모리 상태를 변경했다면 성공 응답 전에 롤백하거나 외부에 성공으로 보이지 않게 해야 한다.

---

## 7. baseline에서 일부러 하지 않는 것

baseline 단계에서는 다음을 의도적으로 제외한다.

- 종목별 lock
- read/write lock
- fine-grained lock
- `epoll`
- hybrid event loop 구조
- request id 기반 exactly-once 재시도 처리
- 고급 backpressure 전략
- 고급 로그 압축 및 group commit

이유는 baseline의 목적이 최적화가 아니라 correctness 검증이기 때문이다.

---

## 8. baseline의 한계

이 baseline은 안정성을 우선하기 때문에 다음 한계를 가진다.

- 읽기와 쓰기가 모두 전역 mutex 아래에서 처리된다
- 상태 변경 요청이 직렬화되어 처리량이 낮아질 수 있다
- thread model 오버헤드가 존재할 수 있다
- 영속 기록을 동기적으로 처리하면 지연이 증가할 수 있다

하지만 이는 baseline 단계에서 의도한 trade-off다.

---

## 9. 한 줄 정리

이 baseline은 Main Thread가 연결을 받고, Worker Thread Pool이 connection 단위로 요청을 반복 처리하며, 전역 상태 저장소를 전역 mutex로 보호하고, 성공한 상태 변경은 append-only log에 기록한 뒤에만 성공 응답을 반환하며, 서버 재시작 시 snapshot과 log replay로 상태를 복구하는 구조다.