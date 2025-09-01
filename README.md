 <img width="464" height="417" alt="image" src="https://github.com/user-attachments/assets/904b6c24-dd8d-4354-9b75-3f850139cdb0" />
<img width="748" height="592" alt="image" src="https://github.com/user-attachments/assets/1f1d22fc-9c00-4d6c-8696-e78eb7dfcb09" />
<img width="714" height="757" alt="image" src="https://github.com/user-attachments/assets/094727c5-ab01-44f2-97c1-41f8e7f74971" />
<img width="261" height="339" alt="image" src="https://github.com/user-attachments/assets/fba80773-41f9-4934-99ad-a11a6a786d64" />
<img width="579" height="158" alt="image" src="https://github.com/user-attachments/assets/dc14f0b2-8f07-42d8-b6e7-45c054b253da" />
<img width="469" height="595" alt="image" src="https://github.com/user-attachments/assets/2a91bd63-c76c-48bd-b855-80b59528981d" />

# ESL Dooray 4 Rooms — 296×128 BWR e-Paper (ESP32, BLE, Dooray API)

Dooray 자원 예약(회의실) 정보를 불러와 296×128 BWR 전자선반라벨(ESL) 태그에 표시하는 ESP32 펌웨어입니다.  
정시/30분마다 업데이트하며, **진행 중 회의는 시간만 빨간색**으로 강조합니다. 데이터 변경/상태 변경/시간 만료(30분) 시에만 BLE 전송을 수행하여 배터리와 전송 시간을 절약합니다.

> ⚠️ **보안 주의:** 예제 코드에 포함된 Dooray API 토큰과 Wi-Fi 정보는 반드시 별도 비공개 파일로 분리하세요. 깃에 토큰이 유출되지 않도록 `.gitignore` 설정을 권장합니다.

---

## 주요 특징

- ⏱ **30분 단위 스마트 업데이트**: 정시(:00) / 반(:30)에만 폴링 및 조건부 화면 갱신
- 🔴 **회의 중 강조**: 현재 진행 중 회의의 **시간(시:분)** 을 RED 레이어로 표시
- 🧠 **변경 감지 해시**: 회의 데이터(MD5)로 변경 여부 판단 → 불필요한 BLE 전송 최소화
- 💾 **NVS 상태 저장**: 태그별 마지막 해시/업데이트 시각/회의중 상태 저장·복구
- 🔄 **KST 시간대 / NTP 동기화**: `kr.pool.ntp.org`, `time.google.com` 사용
- 📶 **네트워크/전송 분리**: Wi-Fi로 Dooray Fetch → Wi-Fi 완전 종료 → BLE로 ESL 업로드
- 🧱 **296×128 BWR 이중 버퍼**: Adafruit_GFX + U8g2 폰트로 BW/RED 별도 캔버스 렌더
- 🔗 **커스텀 BLE 프로토콜**: 이미지 파트 크기 협상, ACK/재전송, 진행률 로그

---

## 데모 화면 규칙

- 상단 좌·중·우: **날짜/시간** · **회의실명** · **상태(회의중/예약가능)**
- 목록: 오늘 회의 최대 6건 표시(초과 시 “... 외 N건”)
- 현재 진행 중 회의: **시간만 빨간색**, 나머지(예약자/제목)는 검정
- 네트워크 실패 시: 하단 `※ 네트워크 오류` 표시

---

## 하드웨어 & 라이브러리

- **ESP32** 개발 보드 (4MB Flash 권장 / Partition: **2MB APP / 2MB SPIFFS, No OTA**)
- 296×128 **BWR e-Paper ESL 태그** (BLE 업로드 지원 모델)
- Arduino Core for ESP32 2.x
- 라이브러리
  - `WiFi.h`, `WiFiClientSecure.h`, `HTTPClient.h`
  - `ArduinoJson.h`
  - `Adafruit_GFX.h`, `U8g2_for_Adafruit_GFX.h`
  - `MD5Builder.h`
  - `Preferences.h`
  - `BLEDevice.h` (NimBLE 아님, ESP32 BLE Classic)
- 폰트: `u8g2_font_unifont_t_korean2` (한글 출력)

---

## 폴더 구조 (예시)

```

/esl-dooray-296x128-bwr
├─ src/
│  └─ main.ino                 # 본문 펌웨어 (이 저장소의 코드)
├─ include/
│  └─ secrets.h                # Wi-Fi/토큰 등 민감정보 (커밋 금지)
├─ README.md
└─ .gitignore

```

`.gitignore` 예시:
```

include/secrets.h
\*.pem

````

---

## 설정 방법

1. **민감정보 분리**
   - `include/secrets.h` 생성:
     ```cpp
     // include/secrets.h
     #pragma once
     static const char* WIFI_SSID = "YOUR-SSID";
     static const char* WIFI_PASS = "YOUR-PASS";
     static const char* DOORAY_TOKEN = "dooray_api_token_here";
     static const char* DOORAY_BASE  = "https://api.dooray.com";
     ```
   - `src/main.ino` 상단의 하드코딩된 값 대신 `#include "secrets.h"`로 교체하세요.

2. **회의실(ESL 태그) 매핑**
   - 코드의 `TAGS[]` 배열에 **ESL BLE MAC**, **Dooray 리소스 ID**, **라벨**을 등록:
     ```cpp
     static Tag TAGS[] = {
       { "FF:FF:92:95:75:78", "3868297860122914233", "소회의실3", "", 0, false, false },
       // { "AA:BB:CC:DD:EE:FF", "RES_ID_2", "소회의실1", "", 0, false, false },
     };
     ```
   - 필요 없는 태그는 주석 처리하거나 삭제.

3. **빌드 설정**
   - Arduino IDE → 보드: ESP32 계열 선택
   - Partition Scheme: **No OTA (2MB APP / 2MB SPIFFS)**
   - Upload Speed: 921600 권장
   - PSRAM: 사용 안 함(필수는 아님)

---

## Dooray API 쿼리

- 엔드포인트:
```

GET {DOORAY\_BASE}/reservation/v1/resource-reservations
?resourceIds={id}\&timeMin={ISO+09:00}\&timeMax={ISO+09:00}

```
- 헤더:
```

Authorization: dooray-api {DOORAY\_TOKEN}
Accept: application/json

```
- 파싱 필터(ArduinoJson): `subject`, `startedAt`, `endedAt`, `users.from.{member|emailUser}.name`

---

## 동작 흐름

1. 부팅 → NVS에서 태그 상태 복구
2. **정시/30분**에만 Dooray Fetch (KST 기준)
3. **변경 감지**: (데이터 해시 변경) OR (회의중 상태 변경) OR (30분 경과) → **업데이트 필요**
4. Wi-Fi 종료 → BLE 초기화 → 각 ESL에 **렌더링/페이로드 빌드/전송**
5. 전송 성공 시 NVS에 해시/시각/상태 저장

---

## BLE 업로드 프로토콜 요약

- Service `0xFEF0`, Char: CMD `0xFEF1`, IMG `0xFEF2`
- 절차
1. `0x01` (파트 크기 협상) → Notify로 `msgSize` 수신 (`partDataSize = msgSize - 4`)
2. `0x02 [len(4B)]` 총 페이로드 길이 전송
3. `0x03` 송신 시작
4. `IMG`에 `[LE32(partIndex) | data...]` 청크 전송
5. CMD Notify `0x05`로 ACK/상태 수신  
   - `0x05 0x00 [ack(LE32)]` → 다음 파트 요청  
   - `0x05 0x08` → 이미지 수신 완료
- 타임아웃/재전송: 마지막 청크 1.5s 내 미ACK 시 재전송(최대 N회)

---

## 페이로드 포맷 (296×128, BWR)

- **열 우선, 상단→하단, BW 후 RED** 순서로 직렬화
- 각 열 128픽셀 → 16바이트(세로 8픽셀 × 16)
- `MIRROR_X = true` → 좌우 반전 적용
- 픽셀 비트 해석:
- BW: `ONE_IS_WHITE = true`
- RED: `ONE_IS_WHITE = false`
- 총 크기: `296열 × 16B × 2층 ≈ 9472B`

---

## 로그 예시

```

\[WiFi] Connected: 10.0.0.12
\[Dooray] HTTP 200, 1345 bytes
\[PAYLOAD] 9472 bytes
\[BLE] Try 1/3
\[TX] 0/40
\[TX] 20/40
OK

```

---

## 트러블슈팅

- **시간 동기화 실패**: NTP 서버 방화벽 확인, `ensureTime()` 리트라이 횟수 증가
- **HTTP 401/403**: Dooray 토큰 만료/권한 확인
- **HTTP 200인데 예약이 비어있음**: `timeMin/timeMax`가 KST(+09:00)인지 확인
- **BLE 전송 실패/지연**: 태그 근접, 간섭 최소화, 재시도 횟수/타임아웃 상향
- **메모리 부족**: 폰트/버퍼 축소, 로그 레벨 감소, 불필요한 동적할당 제거
- **화면 좌우 반전**: `MIRROR_X` 플래그 조정

---

## 커스터마이징 팁

- **회의실 추가**: `TAGS[]` 항목 추가 (MAC/리소스ID/라벨)
- **표시 규칙**: `drawScreen()` 내 문자열 너비 제한, 빨간색 강조 범위 조정
- **업데이트 주기**: `loop()`에서 정각/30분 외 시나리오 추가 가능
- **Dooray 필드 확장**: 참여자/장소 등 필드 파싱 확장

---

## 보안 권장사항

- 토큰/SSID는 `include/secrets.h`로 분리하고 커밋 금지
- 유출 시 **즉시 토큰 폐기/재발급**
- 펌웨어 배포 시 테스트 토큰과 운영 토큰 분리

---

## 라이선스

MIT (예시). 필요 시 조직 정책에 맞게 변경하세요.

---

## 변경 이력

- **v4.0**: 30분 스마트 업데이트, 진행중 시간 RED 강조, 데이터 해시 기반 전송 최적화, NVS 상태 저장, BLE 재시도·ACK 처리 강화
 
