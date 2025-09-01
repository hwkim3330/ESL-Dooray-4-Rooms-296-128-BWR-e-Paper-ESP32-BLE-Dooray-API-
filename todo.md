 

# ✅ 꼭 권하는 5가지 개선

## 1) 캔버스 동적 생성/해제 → **상주(reuse)로 힙 파편화 방지**

지금은 `processUpdates()`마다 `new`/`delete`로 캔버스를 만들었다가 해제합니다. 장시간 돌리면 힙 파편화로 실패 확률이 올라가요. **setup에서 1회 생성**하고 끝까지 재사용하세요.

```cpp
// 전역: 그대로 유지
static GFXcanvas1* canvasBW = nullptr;
static GFXcanvas1* canvasRED = nullptr;

// setup()에서만 1회 생성
void setup() {
  ...
  if(!canvasBW)  canvasBW  = new GFXcanvas1(DRAW_W, DRAW_H);
  if(!canvasRED) canvasRED = new GFXcanvas1(DRAW_W, DRAW_H);
  ...
}

// processUpdates()에서 initCanvas()/freeCanvas() 호출 제거
static void processUpdates(bool forceUpdate = false) {
  Serial.println("\n═══ Update Process ═══");
  // initCanvas();    // 삭제
  ...
  // freeCanvas();    // 삭제
}
```

> 메모리 안정성이 **확** 좋아집니다.

---

## 2) JSON 용량 여유 ↑ (예약 많을 때 안전)

`StaticJsonDocument<4096>`은 빠듯할 수 있어요. Dooray 응답이 많은 날(회의가 많은 날) overflow 가능. **8\~12KB** 정도로 키우거나 동적으로:

```cpp
// ① 간단: 용량 상향
StaticJsonDocument<12288> doc;

// ② 동적 문서 (권장)
DynamicJsonDocument doc(12288);
```

필터는 이미 잘 쓰셔서 CPU/메모리 모두 효율적입니다.

---

## 3) ISO 시간대 처리 강화 (UTC ‘Z’/+09:00 대응)

현재 `parseISO()`는 `YYYY-MM-DDTHH:MM:SS`까지만 읽고 **타임존 오프셋 무시**라, Dooray가 UTC로 주면 오차가 날 수 있어요. 간단한 오프셋 보정을 넣어주세요.

```cpp
static time_t parseISO(const char* iso, uint8_t& h, uint8_t& m){
  if(!iso || strlen(iso) < 19) return 0;

  // 기본 시각 파싱
  int Y,Mo,D,Ho,Mi,Se;
  if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &Y,&Mo,&D,&Ho,&Mi,&Se) != 6) return 0;

  // 오프셋 탐지: 'Z' 또는 +HH:MM / -HH:MM
  int sign = 0, offH = 0, offM = 0;
  const char* tz = iso + 19;
  if (*tz == 'Z') { sign = 0; } // UTC
  else if (*tz == '+' || *tz == '-') {
    sign = (*tz == '+') ? +1 : -1;
    // 형식: +09:00
    if (sscanf(tz+1, "%2d:%2d", &offH, &offM) != 2) { offH = offM = 0; sign = 0; }
  }

  struct tm tmv = {};
  tmv.tm_year = Y - 1900;
  tmv.tm_mon  = Mo - 1;
  tmv.tm_mday = D;
  tmv.tm_hour = Ho;
  tmv.tm_min  = Mi;
  tmv.tm_sec  = Se;

  // 입력이 UTC기준이면(‘Z’ 또는 +hh:mm), KST로 맞추기 위해 오프셋 반영
  // iso가 로컬(KST)이라면 sign=0/offH=offM=0로 들어와 영향 없음
  time_t t = mktime(&tmv);  // 현재 TZ(KST) 기준
  if (*tz == 'Z' || sign != 0) {
    // UTC시간을 로컬(KST)로 바꾼다고 가정: KST(+9) 기준 보정
    // 1) UTC 기준 epoch으로 맞추려면 timegm()이 이상적이나, ESP32엔 없음
    // 2) 근사치: 먼저 UTC로 보정한 뒤 로컬 변환을 다시 사용하는 대신, 직접 +09:00로 더해줌
    //    간단 대응: KST(+9h) - (부착된 오프셋)
    int totalMin = 9*60 - (sign * (offH*60 + offM));
    t += totalMin * 60;
  }

  struct tm lt; localtime_r(&t, &lt);
  h = lt.tm_hour; m = lt.tm_min;
  return t;
}
```

> Dooray가 이미 `+09:00`로 반환하면 **영향 없음**, `Z`(UTC)로 오면 정확해집니다.

---

## 4) Wi-Fi OFF 시 **deinit 에러 무시** (가끔 상태 레이스)

간혹 `esp_wifi_deinit()`이 이미 deinit된 상태라 에러를 뱉기도 합니다. 리턴값 체크 후 무시하세요.

```cpp
static void wifiOff(){
  WiFi.disconnect(true, true);
  delay(300);
  esp_wifi_stop();               // 이미 stop상태여도 OK
  delay(200);
  esp_err_t e = esp_wifi_deinit();
  (void)e; // 에러여도 무시
  delay(200);
  WiFi.mode(WIFI_OFF);
  delay(500);
}
```

---

## 5) “무조건 갱신”은 그대로, **fetch 실패해도 시계는 그리기**

지금은 Dooray 실패 시 `continue;` 해서 이후 로직이 스킵될 수 있습니다.
**데이터는 실패해도 시계는 항상 갱신**되도록, `drawScreen()`/`buildPayload()`/BLE 업로드까지는 타고 가되 `rd.ok=false` 상태를 화면 하단 경고로 유지하세요. (이미 경고 그려주시는 로직이 있어서, `continue`만 줄이면 됩니다)

```cpp
bool fetchOk = fetchReservationsToday(TAGS[i].resId, g_room[i]);
if (!fetchOk) {
  Serial.println("FAIL");
  // continue;   // <= 이 줄 삭제 → 예약 리스트는 비어있지만 날짜/시계/상태는 다시 그림
}
...
// forceUpdate가 true면 무조건 needsUpdate = true로
bool isBusy = (getCurrentMeetingIdx(g_room[i]) >= 0);
bool dataChanged = (strcmp(g_room[i].dataHash, TAGS[i].lastHash) != 0);
bool statusChanged = (TAGS[i].lastBusyState != isBusy);
bool timeExpired = (now - TAGS[i].lastUpdate) > (30 * 60);

if (forceUpdate || dataChanged || statusChanged || timeExpired || !fetchOk) {
  TAGS[i].needsUpdate = true;
  TAGS[i].lastBusyState = isBusy;
  updateCount++;
  Serial.printf("UPDATE (data:%d, status:%d, fetchOk:%d)\n", dataChanged, statusChanged, fetchOk);
} else {
  TAGS[i].needsUpdate = false;
  Serial.println("SKIP");
}
```

> 이렇게 하면 **네트워크가 잠깐 죽어도 시계는 정확히 30분마다 갱신**됩니다.

---

# ⚙️ 기타 마이크로 최적화(선택)

* `strcpy`/`strncpy` 대신 **`strlcpy`** 사용(버퍼 오버런 방지 가독↑).
* `MD5Builder.toString()` → `strlcpy(hash, md5.toString().c_str(), 33);`
* Dooray 토큰/SSID 앞뒤 공백 방지: 환경에 따라 앞 공백 `" Q"`가 그대로 헤더에 실려 401이 날 수 있으니 **트림** 확인.
* `HTTPClient`에 `Connection: close` 헤더 추가(이미 `setReuse(false)`지만, FW에 따라 도움이 될 때가 있음).
* BLE 재시도: 2회 → 3회로 늘리고, 실패 시 1초→2초로 지수 백오프.

```cpp
http.addHeader("Connection","close");
```

---

# 🔚 결론

* **정시/30분 강제 갱신** 설계 자체는 아주 좋아요.
* 위 5가지(특히 **캔버스 상주**, **JSON 여유**, **ISO TZ 보정**, **continue 제거**)만 반영하면
  장시간 무정지 운용성과 “시계는 항상 정확히 갱신”이 더 단단해집니다.
 
