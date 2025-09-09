 

# ✅ v4.3-Lite TODO

## P0 — 네트워크/안정화

* [ ] **HTTP 헤더 순서 수정**: `http.begin()` 이후 `addHeader()` 호출로 고정

  * 수용기준: 100회 연속 호출 중 401/403/-1 무발생(로컬 로그 기준)
* [ ] **Dooray GET 재시도/백오프(×3)** 도입

  * 수용기준: 일시 네트워크 오류 시 2회 이내 자복구 로그 확인
* [ ] **Wi-Fi deinit 제거**: `wifiOff()`에서 `esp_wifi_stop()`까지만, 유지시간 60s

  * 수용기준: 24h 구동 중 `WiFi.status()!=WL_CONNECTED` 진입률 < 1%
* [ ] **동시 실행 락**: `g_updating` 플래그로 `processUpdates()`/`poll…()` 상호 배제

  * 수용기준: 강제업데이트와 5분 폴링 충돌 로그 무

## P0 — 시간/파서/메모리

* [ ] **ISO8601 TZ 보정**: `parseISO()`에서 `Z/±hh:mm → KST` 보정 코드 반영

  * 수용기준: UTC 응답/ +09:00 응답 모두 같은 현지시 표시
* [ ] **JSON 버퍼 여유**: `DynamicJsonDocument(16384)`로 상향

  * 수용기준: 예약 12건+ 시 `deserializeJson()` OOM 무

## P0 — 캔버스 상주/항상 갱신

* [ ] **캔버스 상주화**: `setup()` 1회 `new`, 이후 **해제 금지**
* [ ] **fetch 실패 시에도 렌더**: `continue` 제거 → 시계/상태 항상 재그리기

  * 수용기준: WAN 끊김 상태에서도 30분마다 시계 갱신 확인

## P0 — 전송 신뢰/속도

* [ ] **BLE MTU/데이터 길이 확장** (가급적 NimBLE 전환, 아니면 기존 BLE에 MTU 517 시도)

  * 수용기준: 페이로드(≈9.3KB) 업로드 시간 30%↓
* [ ] **청크 타임아웃 단축**: 1500ms → 600ms, 재전송 최대 10회

  * 수용기준: ACK 정체 시 자동 회복, 전체 업로드 타임아웃 60s 이내 유지

---

## P1 — 렌더링/가독성/UX

* [ ] **극성 확정**: `BW_ONE_IS_WHITE=false`, `RED_ONE_IS_WHITE=true` 기본값으로 시험

  * 수용기준: 검정/적색 표현 기대치와 일치
* [ ] **적색 배지 그리기 규칙 통일**: `canvasRED->fillRect(..., 1)` + `u8g2.setForegroundColor(1)`

  * 수용기준: “회의중” 배지 콘트라스트 안정
* [ ] **페이크 Bold & 행간 확대**: 상단 날짜/회의실/상태 큰 글씨(이중 드로잉+행간 18\~20px)

  * 수용기준: 1m 이격 시 가독성 체감 ↑
* [ ] **진행중 항목 상단 고정(옵션)**: 리스트 밖(>6)일 때 상단에 한 줄 요약 표시

  * 수용기준: 진행중 정보 항상 시야 내

## P1 — 완전 리프레시(화이트 플래시)

* [ ] **화이트 프레임 선행 업로드(조건부)**: 현재 “회의중”일 때 `올화이트→실화면` 2연속 전송

  * 수용기준: 최종 반영 전에 하얀 플래시 확실히 보임(모델에 따라 토글)

---

## P2 — 코드 경량/정리

* [ ] **페이로드 빌더 단순화**: 루프/인덱스 축소, `reserve(DRAW_W*32)`
* [ ] **문자열 안전화**: `strlcpy` 일원화, 불필요 로그 정리(레벨 스위치)
* [ ] **상수화**: 재시도/타임아웃/유지시간 `#define`로 노출

---

# 🔒 보안/환경

* [ ] **secrets 분리**: `include/secrets.h` + `.gitignore`
* [ ] **CA 적용 검토**: 운영 빌드에서 `setCACert()` 또는 핀닝(가능 시)
* [ ] **헤더 공백 트림**: 토큰/SSID 앞뒤 공백 방지

---

# 🧪 테스트 계획(요약 체크리스트)

* [ ] **기동 안정성**: 부팅 후 첫 fetch 성공률 ≥ 99%
* [ ] **24h 연속 운전**: 메모리 누수/프리힙 안정 로그
* [ ] **WAN 교란**: DNS 실패/5xx/타임아웃 주입 시 3회 내 회복
* [ ] **전송 성능**: MTU 확장 전/후 업로드 소요 비교 로그
* [ ] **표시 확인**: RED 배지/화이트 플래시/진행중 고정줄

---

# 🚀 배포/롤백

* [ ] **버전 태깅**: `v4.3-lite` 브랜치/릴리스
* [ ] **릴리즈 노트**: 변경점·리스크·시험결과
* [ ] **롤백 스위치**: `USE_NIMBLE`/`FORCE_WHITE_FRAME` 컴파일 플래그로 on/off

---

## 참고 구현 포인트(짧은 스니펫)

**헤더 순서**

```cpp
if (!http.begin(cli, url)) return false;
http.addHeader("Authorization", authHdr);
http.addHeader("Accept", "application/json");
http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
```

**GET 재시도**

```cpp
int code=-1; String body;
for(int a=0;a<3;++a){ code=http.GET(); if(code==200){ body=http.getString(); break; } delay(250*(a+1)); }
if(code!=200 || body.isEmpty()) { http.end(); return false; }
```

**동시 실행 락**

```cpp
static volatile bool g_updating=false;
if (g_updating) return;
g_updating=true; /* ... */ g_updating=false;
```

**RED 배지**

```cpp
u8g2.begin(*canvasRED); u8g2.setForegroundColor(1);
canvasRED->fillRect(bx, by, sw+14, 20, 1);
```

**화이트 프레임(옵션)**

```cpp
canvasBW->fillScreen(1); canvasRED->fillScreen(1);
buildPayload(); /* tryProtocolA()로 1회 업로드 */
```
 
