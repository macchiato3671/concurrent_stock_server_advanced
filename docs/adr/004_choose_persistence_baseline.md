# ADR-004: Manifest 기반 파일 영속성 baseline 채택

- Status: Accepted
- Date: 2026-03-31

## Context

동시성 주식 서버의 baseline 단계에서 서버 재시작 후에도 상태를 복구할 수 있는 파일 기반 영속성이 필요했다. 초기 방향은 init/snapshot 파일을 base로 사용하고 append-only log(AOF)를 replay하는 구조였다. 그러나 단순히 "snapshot이 있으면 snapshot 사용, 없으면 init 사용, 그리고 AOF 전체 replay" 방식만으로는 snapshot 저장과 AOF reset 사이의 중간 실패 상태를 안전하게 처리하기 어려웠다. 특히 새 snapshot 저장에는 성공했지만 AOF reset이 실패한 경우, 다음 시작 시 snapshot과 이전 AOF가 함께 읽혀 중복 replay가 발생할 수 있었다. 

또한 recovery 도중 일부만 반영된 상태가 live store에 남지 않도록 해야 했고, base 상태 파일(init/snapshot)에서 같은 stock id가 중복되는 경우 조용히 overwrite하는 대신 오류로 처리할 필요가 있었다. 최신 코드에서는 recovery를 temp store에 수행한 뒤 성공 시 swap하고 base 파일 중복 id를 invalid로 처리하는 방향으로 구조가 정리되었다.

## Decision

다음과 같은 파일 영속성 baseline을 채택한다.

### 1. Manifest 기반 generation 관리
snapshot과 AOF를 단일 파일 쌍이 아니라 generation 단위의 세트로 관리한다.

예:
- `stock.snapshot.<gen>`
- `stock.aof.<gen>`
- `stock.manifest`

`stock.manifest`는 현재 공식 상태로 인정되는 generation 하나를 가리킨다. recovery는 단순히 존재하는 파일을 읽는 것이 아니라 manifest가 가리키는 snapshot/AOF 세트만 읽는다. 

### 2. Recovery는 manifest가 가리키는 generation을 기준으로 수행
recovery는 시작 시 `stock.manifest`를 읽어 현재 공식 상태로 인정되는 active generation을 결정한다.  
그 뒤 해당 generation의 snapshot과 AOF를 순서대로 읽어 `live_store`에 복구한다.

절차:
1. manifest에서 active generation을 읽는다.
2. 해당 generation의 snapshot 경로와 AOF 경로를 구성한다.
3. snapshot을 `live_store`에 로드한다.
4. 해당 generation의 AOF를 같은 `live_store`에 순서대로 replay한다.

manifest가 없으면 bootstrap 상황으로 본다.
- `stock.bootstraping` marker가 있으면 이전 incomplete bootstrap 흔적을 정리하고 marker를 제거한 뒤 다시 bootstrap을 진행한다.
- marker가 없으면 persistence artifact가 전혀 없는 경우에만 init 파일을 base로 bootstrap을 시작한다.
- persistence artifact가 남아 있는데 manifest가 없으면 애매한 상태로 간주하고 시작을 실패시킨다.

### 3. Bootstrap 진행 상태는 marker 파일로 구분
첫 bootstrap 또는 bootstrap 재시도 중인 상태를 명시적으로 구분하기 위해 `stock.bootstraping` marker 파일을 사용한다.

원칙:
1. manifest가 없고 marker도 없으면, persistence artifact가 전혀 없는 경우에만 init 파일 기반 bootstrap을 시작한다.
2. bootstrap 시작 전에 marker를 생성하고 부모 디렉터리를 fsync한다.
3. bootstrap이 성공적으로 끝나고 manifest까지 publish된 뒤 marker를 제거하고 부모 디렉터리를 fsync한다.
4. 다음 시작 시 manifest는 없지만 marker가 남아 있으면, 이전 bootstrap이 중간 실패한 상태로 간주한다.
5. 이 경우 이전 incomplete bootstrap 흔적을 정리한 뒤 marker를 제거하고 다시 bootstrap을 진행한다.

이 marker는 "manifest가 아직 publish되지 않았지만 bootstrap이 진행 중이었음"을 나타내는 보조 상태 표식이다. 따라서 recovery는 manifest만이 공식 상태를 가리킨다는 원칙을 유지하면서도, 첫 bootstrap 중간 실패와 일반적인 orphan artifact 상황을 구분할 수 있다.

### 4. Persist는 새 generation 세트를 만든 뒤 manifest를 마지막에 갱신
persist 시 기존 세트를 먼저 지우지 않는다. 먼저 새 generation의 snapshot과 AOF를 준비한 뒤, 마지막에 manifest를 갱신한다.

절차:
1. 새 generation 번호를 결정한다.
2. 새 generation의 snapshot 파일을 최종 경로에 직접 생성하고 내용을 기록한 뒤 file fsync와 부모 디렉터리 fsync를 수행한다.
3. 새 generation의 empty AOF 파일을 최종 경로에 직접 생성하고 file fsync와 부모 디렉터리 fsync를 수행한다.
4. manifest temp 파일에 새 generation을 기록하고 fsync한 뒤, rename으로 `stock.manifest`에 publish한다.
5. manifest publish 후 부모 디렉터리 fsync를 수행한다.
6. 마지막에 이전 generation 파일들을 정리(cleanup)한다.

즉, manifest는 "현재 공식 세트"를 승인하는 마지막 스위치 역할을 한다. snapshot이나 AOF가 먼저 만들어지더라도 manifest가 갱신되기 전까지는 새 generation이 공식 상태가 아니다. 따라서 publish 중간 실패가 발생해도 recovery는 여전히 이전 manifest가 가리키는 generation을 기준으로 수행할 수 있다.

또한 old generation cleanup은 manifest 갱신 이후의 후속 정리 단계이며, 실패하더라도 현재 공식 generation 자체는 바뀌지 않는다.

### 5. Directory fsync는 artifact 종류에 따라 다르게 반영
최신 코드에서는 부모 디렉터리 fsync를 반영하고 있지만, 모든 파일이 동일한 publish 절차를 따르는 것은 아니다.

- snapshot 파일:
  1. 최종 경로 파일 open/write
  2. file fsync
  3. close
  4. 부모 디렉터리 fsync

- AOF 파일:
  1. 최종 경로 파일 open
  2. file fsync
  3. close
  4. 부모 디렉터리 fsync

- manifest 파일:
  1. temp 파일 write
  2. temp 파일 fsync
  3. `rename(tmp, final)`
  4. 부모 디렉터리 fsync

이는 파일 내용뿐 아니라 create, rename, unlink에 따른 디렉터리 엔트리 변경까지 더 엄밀하게 반영하기 위한 것이다. 특히 manifest는 temp + rename + directory fsync를 통해 authoritative generation 전환을 publish한다.

## Consequences

- snapshot 저장 성공 후 AOF reset 실패로 인한 중복 replay 문제를 manifest 기반 generation 관리로 완화할 수 있다.
- 어떤 snapshot/AOF 조합이 현재 공식 상태인지 manifest를 통해 명확하게 결정할 수 있다.
- 새 snapshot/AOF가 먼저 생성되더라도 manifest가 갱신되기 전까지는 공식 상태가 바뀌지 않는다.
- snapshot, AOF, manifest publish 과정에서 file fsync 및 부모 디렉터리 fsync를 반영해 내구성을 강화한다.
- old generation cleanup이 실패하더라도 manifest가 이미 현재 generation을 가리키므로, recovery 기준 자체는 유지된다.

### 범위 제한
이 ADR은 파일 기반 영속성과 recovery correctness에 대한 baseline 결정을 다룬다. 다음 항목은 현재 범위에 포함하지 않는다.

- request_id 기반 exactly-once 처리
- 더 복잡한 rollback 정책
- orphan artifact 자동 복구 고도화
- BST 대체 자료구조
- worker pool / bounded queue / protocol 구현

## Alternatives Considered

### 대안 1. snapshot + 단일 AOF reset 방식 유지
snapshot 저장 후 기존 AOF를 비우는 단순 구조를 유지하는 방법이다.

채택하지 않은 이유:
- snapshot 성공 + AOF reset 실패 시 snapshot과 이전 AOF의 조합이 남을 수 있음
- 다음 recovery에서 중복 replay 가능성이 있음

### 대안 2. 파일 존재 여부만 보고 recovery
manifest 없이 snapshot/AOF 파일이 존재하면 그대로 사용하는 방법이다.

채택하지 않은 이유:
- 어떤 snapshot과 어떤 AOF가 같은 시점의 세트인지 판별할 수 없음
- 중간 실패 상태를 안전하게 구분하기 어려움