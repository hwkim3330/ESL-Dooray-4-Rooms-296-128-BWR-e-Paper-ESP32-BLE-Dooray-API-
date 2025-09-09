

# ESL Dooray 4 Rooms — 296×128 BWR e-Paper (ESP32, BLE, Dooray API)

## 기술문서 v4.2 (Enhanced)

## 1. 개요

### 1.1 목적

Dooray 자원예약(회의실) 정보를 **ESP32 펌웨어**로 수집·가공하여 **296×128 BWR e-Paper ESL 태그**에 실시간에 가깝게 표시한다. 배터리·전송비용을 줄이기 위해 **30분 강제+5분 폴링 기반의 조건부 BLE 갱신**을 수행한다.

### 1.2 범위

* 하드웨어: ESP32, 296×128 BWR ESL(2-plane: BW/RED, BLE 이미지 업로드 지원)
* 네트워크/백엔드: Dooray Reservation API (OAuth 토큰 방식), NTP(KST)
* 표시: 한국어 폰트(U8g2), 현재 회의 **시간만 RED 강조**
* 전송: 커스텀 **BLE 단편(Part) 전송/ACK 재시도** 프로토콜

### 1.3 용어

* **ESL**: Electronic Shelf Label / e-Paper 태그
* **KST**: Korea Standard Time (UTC+09:00)
* **BWR**: Black-White-Red 3색 e-Paper
* **MD5 해시**: 회의목록 변경 감지용 요약값
* **NVS(Preferences)**: 마지막 상태 저장(해시/업데이트시각/회의중 여부)

---

## 2. 상위 요구사항(요약)

| ID          | 요구사항          | 설 명                                   | 코드 기준                              |
| ----------- | ------------- | ------------------------------------- | ---------------------------------- |
| REQ-TIME    | KST 기준 시간 동기화 | NTP 2원화 & 6시간 주기 재동기화                 | `setupSNTP()`, `ensureTime()`      |
| REQ-FETCH   | Dooray 일정 조회  | 당일 범위 ISO+09:00, 필드 필터링 후 최소 파싱       | `fetchReservationsToday()`         |
| REQ-RENDER  | 화면 렌더링        | 상단 날짜/회의실/상태, 진행중 시간 RED 강조, 최대 6건 목록 | `drawScreen()`                     |
| REQ-DELTA   | 변경감지/조건부 전송   | (데이터 변경) OR (상태 변화) OR (30분 경과)       | `processUpdates()`                 |
| REQ-BLE     | 신뢰성 전송        | Part 크기 협상, ACK 기반 재전송, 3회 백오프        | `tryProtocolA()`                   |
| REQ-PERSIST | 상태 영속화        | lastHash/lastUpdate/lastBusyState     | `saveTagState() / loadTagStates()` |
| REQ-POLL    | 5분 폴링         | 변경 감지 시 즉시 업데이트 트리거                   | `pollDoorayForChanges()`           |
| REQ-SAFETY  | 네트워크/자원 분리    | Wi-Fi fetch → Wi-Fi 완전 종료 → BLE 업로드   | `wifiOnAndConnect()/wifiOff()`     |

---

## 3. 시스템 아키텍처

### 3.1 블록 개요

```
[Dooray API]  <--HTTPS-->  [ESP32 Wi-Fi]
                                   |
                                (JSON 파싱 → MD5 해시)
                                   |
                              [렌더링 엔진]
                      (Adafruit_GFX + U8g2, BW/RED 캔버스 상주)
                                   |
                          [페이로드 생성(9472B)]
                                   |
                                [ESP32 BLE]
                       (FEF0/FEF1/FEF2 커스텀 프로토콜)
                                   |
                             [ESL e-Paper Tag]
```

### 3.2 주기/트리거

* **강제 업데이트**: 매 정시(:00)·30분(:30)
* **폴링**: 매 5분(변경 감지 시 즉시 업데이트)
* **타임아웃/재시도**: BLE 파트 전송 1.5s 무응답 시 재전송, 시도 3회 백오프

---

## 4. 상세 설계

### 4.1 데이터 구조

```cpp
struct Resv {
  char subj[80], who[36];
  uint8_t startH, startM, endH, endM;
  time_t st=0, ed=0;
};

struct RoomData {
  Resv list[12];
  uint8_t count=0;
  bool ok=false;
  char dataHash[33]; // MD5 hex
};

struct Tag {
  const char* mac;    // ESL BLE MAC
  const char* resId;  // Dooray resourceId
  const char* label;  // 표기명
  char lastHash[33];
  uint32_t lastUpdate;
  bool needsUpdate;
  bool lastBusyState;
};
```

* **MD5 해시 방식**: `start/end 시각 + who + subj`를 누적해 MD5 → 변경 감지
* **NVS 키**: `tag{i}`(해시), `tag{i}t`(시각), `tag{i}b`(회의중 여부)

### 4.2 시간 처리(KST)

* `setTZ_KST()` 로컬 TZ 설정, `configTzTime()`으로 NTP 2원화
* ISO8601(+09:00/’Z’/±hh\:mm) 파싱 후 **KST 보정**: `parseISO()`

### 4.3 Dooray API 연동

* 엔드포인트:
  `GET {DOORAY_BASE}/reservation/v1/resource-reservations?resourceIds={id}&timeMin={ISO+09:00}&timeMax={ISO+09:00}`
* 헤더: `Authorization: dooray-api {TOKEN}`, `Accept: application/json`
* **필드 필터 파싱**(메모리 절감): `subject, startedAt, endedAt, users.from.{member|emailUser}.name`
* 결과를 `Resv`로 정규화, 시작시간 기준 정렬

### 4.4 렌더링

* **캔버스 상주**: `GFXcanvas1` BW/RED 2장(힙 파편화 회피)
* 상단: 날짜/시간(: 또는 .), 회의실명(센터), 상태(우측)
* 목록: 최대 6건, 초과 시 `"... 외 N건"`
* **현재회의 RED 강조**: *시간 문자열만* RED 레이어에 그림
* 네트워크 실패 시 바닥줄 `※ 네트워크 오류` 표시

### 4.5 페이로드 직렬화(약 9,472B)

* 해상도 296×128, **열 우선/상→하** 16바이트(128px/8) × 296열 × **2층(BW→RED)**
* 미러: `MIRROR_X = true` (좌우 반전)
* 비트 의미:

  * BW: `ONE_IS_WHITE = true`
  * RED: `ONE_IS_WHITE = false`
* 구현: `buildPayload()` → `g_payload`

### 4.6 BLE 업로드 프로토콜

* **Service**: 0xFEF0 / **CMD**: 0xFEF1 / **IMG**: 0xFEF2
* 절차(`tryProtocolA()`):

  1. `0x01`(파트 크기 협상) → Notify 수신: `msgSize`(=partMsgSize), `partDataSize = msgSize - 4`
  2. `0x02 [LE32(total_len)]`
  3. `0x03` 송신 시작
  4. **IMG**에 `[LE32(partIndex) | data…]` 반복 전송
  5. **CMD Notify 0x05**:

     * `0x05 0x00 [LE32 ackPart]` → 다음 파트 요청
     * `0x05 0x08` → 수신 완료
* **신뢰성**: 마지막 청크 1.5s 내 미ACK 시 재전송(최대 20회 루프 보호), 태그 연결 3회 재시도+백오프

### 4.7 업데이트 결정 로직

```text
for each TAG:
  fetchOk = Dooray 조회 성공 여부
  busyNow = 현재 시각이 어떤 예약 [st, ed) 구간에 속하는지
  dataChanged  = (g_room[i].dataHash != TAGS[i].lastHash)
  statusChanged= (TAGS[i].lastBusyState != busyNow)
  timeExpired  = (now - TAGS[i].lastUpdate >= 30분)

  needsUpdate = forceUpdate || dataChanged || statusChanged || timeExpired || !fetchOk
```

* Wi-Fi fetch 후 **Wi-Fi 완전 종료** → BLE 업로드 (무선 간섭/전류 피크 최소화)

---

## 5. 오류/장애 대응 설계

| 상황             | 감지/로그                               | 즉시 조치                   | 후속 조치              |
| -------------- | ----------------------------------- | ----------------------- | ------------------ |
| NTP 실패         | `[JSON] Error`, `ensureTime()` 타임아웃 | Wi-Fi 유지 중 재시도          | 6h 주기 재동기화, 부팅 재시도 |
| Dooray HTTP 오류 | `[Dooray] HTTP {code}`              | fetch 실패로 표기, 화면 하단 경고  | 5분 폴링으로 변동 재확인     |
| JSON 파싱 오류     | `[JSON] Error: …`                   | 해당 태그 `ok=false`, 안전 표시 | 다음 강제/폴링 시 재시도     |
| BLE 연결 실패      | `[BLE] Failed`                      | tag별 3회 재시도+백오프         | 다음 사이클에서 재시도       |
| BLE 전송 정체      | 마지막 청크 1.5s 무ACK                    | 동일 청크 재전송(정체 카운트)       | 20회 초과 시 실패 처리     |
| 메모리 부족         | FreeHeap 로그                         | 폰트/버퍼 축소 고려             | 로그/폰트 최적화 가이드 적용   |

---

## 6. 보안/비밀정보 처리

* **토큰/SSID 분리**: `include/secrets.h` 로 분리, `.gitignore` 적용
* **유출 대응**: 토큰 노출 시 즉시 폐기/재발급, 운영/테스트 토큰 분리
* HTTPS는 `WiFiClientSecure::setInsecure()` 사용. 배포 환경에서는 **핀닝/CA 적용** 권장

---

## 7. 구성/설정

### 7.1 태그 매핑

```cpp
static Tag TAGS[] = {
  { "FF:FF:92:95:73:06", "3868312518617103681", "소회의실1", "", 0, false, false },
  { "FF:FF:92:95:73:04", "3868312634809468080", "소회의실2", "", 0, false, false },
  { "FF:FF:92:95:95:81", "3868312748489680534", "중회의실1", "", 0, false, false },
};
```

* 필요 태그만 유지, MAC/Resource ID/라벨 확인

### 7.2 빌드 권장값

* Partition: **No OTA (2MB APP / 2MB SPIFFS)**
* Upload Speed: 921600, PSRAM 미사용(선택)

---

## 8. 시험(Verification) 계획

### 8.1 기능 시험 항목

1. **시간 동기화**: 부팅 후 20회 내 `getLocalTime()` 성공
2. **Dooray Fetch**: 200 응답/빈 목록/401/403/네트워크 단절 케이스
3. **변경 감지**: 제목/예약자/시간 변경 시 MD5 변화 감지
4. **표시 규칙**: 진행 중 회의의 **시간만** RED, 6건 초과 시 `… 외 N건`
5. **전송 신뢰성**: ESL 1\~3m 거리, 간섭 환경에서 ACK 누락 재전송 동작
6. **전원/메모리**: FreeHeap 로그 확인, 장시간(≥48h) 운전

### 8.2 성능 목표

* 일반 사이클(변경 無)에서 **BLE 전송 0회**
* 변경 발생 시 **1분 내 반영**(최대 5분 폴링 간격 + 즉시 업데이트 경로)

---

## 9. 운용 가이드(트러블슈팅 요약)

* **시간이 ‘--’로 보임** → NTP 방화벽/네트워크 점검 → 부팅 재시도
* **항상 ‘예약 없음’** → `timeMin/Max`가 KST(+09:00)인지 확인
* **화면 좌우 반전** → `MIRROR_X`를 `false`로 변경
* **마지막 흰색 플래시 부족감** → (하드웨어 종속) 이미지 완주 후 ESL 자체 리프레시 타이밍 확인 필요. 전송 완료(0x05 0x08) 직후 약간의 **settle delay**(e.g., 500\~800ms) 삽입을 고려.

  * 적용 지점: `tryProtocolA()`에서 완료 검출 후 `delay(600);` 추가 검토

---

## 10. 변경 이력(요약)

* **v4.2 (Enhanced)**:

  * 30분 강제 + 5분 폴링 → **즉시 반영 경로**
  * 캔버스 **상주화**(힙 파편화 방지), JSON 필터링 최적화
  * BLE **ACK/재전송** 안정화, **백오프** 도입
  * NVS에 **해시/시각/상태** 저장

---

## 부록 A. 시퀀스(텍스트)

**정시/30분 강제 갱신 시**

1. `wifiOnAndConnect()`
2. `ensureTime()` → `fetchReservationsToday()` × N
3. `needsUpdate` 판정 → Wi-Fi 종료(`wifiOff()`)
4. `BLEDevice::init()` → 태그별 `connectAndDiscover()`
5. `drawScreen()` → `buildPayload()` → `tryProtocolA()`
6. 성공 시 `saveTagState()` → `BLEDevice::deinit()`

**5분 폴링 시**

1. `wifiOnAndConnect()` → `fetch…()` × N(가벼운 파싱)
2. 변경 감지 시 `processUpdates(true)` 호출

---

## 부록 B. 보완 제안(코드 레벨)

* **완료 후 안정화 지연**: `tryProtocolA()`에서 `0x05 0x08` 수신 직후 `delay(600)` 추가(일부 ESL 모델의 “마지막 백면 플래시” 신뢰 확보)
* **재시도 한도 노출**: 재시도 횟수/타임아웃을 `#define`으로 상수화
* **HTTPS 강화**: 운영 배포에서 `setCACert()` 또는 핀닝 적용
* **메모리 마진**: `DynamicJsonDocument` 12KB → 실데이터 기반 튜닝

---

### 문서화 노트

* 본 기술문서는 **코드 주석/구현을 1:1 반영**하여 작성되었고, 별첨의 **TSN 기술문서 형식** 중 요구·상태·흐름 서술 방식을 참고해 구성했습니다.&#x20;

