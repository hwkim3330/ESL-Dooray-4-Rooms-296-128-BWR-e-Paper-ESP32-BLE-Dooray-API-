// ============================================================
// ESL 296x128 BWR — Dooray 4 Rooms (강화 통합 v4.2)
// - 매 정시/30분 강제 업데이트 (시계/상태 항상 갱신)
// - 5분 간격 Dooray 폴링 → 변경 감지 시 즉시 업데이트
// - 캔버스 상주(힙 파편화 방지), ISO8601 TZ 보정, JSON 여유 확대
// - Wi-Fi deinit 안전화, BLE 전송 재시도/백오프
// ============================================================

#include <Arduino.h>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cctype>
#include <Preferences.h>

// ---------- Wi-Fi / HTTP / JSON / Time ----------
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <esp_sntp.h>
#include <esp_task_wdt.h>
extern "C" {
  #include "esp_bt.h"
  #include "esp_wifi.h"
  #include "esp_bt_main.h"
}

// ---------- BLE ----------
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <BLEAddress.h>
#include <BLERemoteCharacteristic.h>

// ---------- Canvas & Font ----------
#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>

// ---------- MD5 ----------
#include <MD5Builder.h>

// ------------------- User config -------------------
static const char* WIFI_SSID = "Smart Global";              // 바꿔 넣기
static const char* WIFI_PASS = "";                          // 바꿔 넣기
static const char* DOORAY_TOKEN = "YOUR_DOORAY_TOKEN_HERE"; // 바꿔 넣기
static const char* DOORAY_BASE  = "https://api.dooray.com";

// ESL 태그 정보
struct Tag {
  const char* mac;
  const char* resId;
  const char* label;
  char     lastHash[33];
  uint32_t lastUpdate;
  bool     needsUpdate;
  bool     lastBusyState;
};

static Tag TAGS[] = {
  // { "FF:FF:92:95:75:78", "3868297860122914233", "소회의실3", "", 0, false, false },
  { "FF:FF:92:95:73:06", "3868312518617103681", "소회의실1", "", 0, false, false },
  { "FF:FF:92:95:73:04", "3868312634809468080", "소회의실2", "", 0, false, false },
  { "FF:FF:92:95:95:81", "3868312748489680534", "중회의실1", "", 0, false, false },
};
static constexpr int NUM_TAGS = sizeof(TAGS)/sizeof(TAGS[0]);

// ------------------- Canvas (상주) -------------------
static constexpr int DRAW_W = 296, DRAW_H = 128;
static GFXcanvas1* canvasBW = nullptr;
static GFXcanvas1* canvasRED = nullptr;
static U8G2_FOR_ADAFRUIT_GFX u8g2;

static const bool MIRROR_X         = true;
static const bool BW_ONE_IS_WHITE  = true;
static const bool RED_ONE_IS_WHITE = false;

// ------------------- Data Types -------------------
struct Resv {
  char subj[80];
  char who[36];
  uint8_t startH, startM, endH, endM;
  time_t st=0, ed=0;
};

struct RoomData {
  Resv list[12];
  uint8_t count=0;
  bool ok=false;
  char dataHash[33];
};
static RoomData g_room[NUM_TAGS];

static Preferences prefs;

// ------------------- NVS 저장/로드 -------------------
static void saveTagState(int idx) {
  char key[16], keyTime[16], keyBusy[16];
  snprintf(key,     sizeof(key),     "tag%d",  idx);
  snprintf(keyTime, sizeof(keyTime), "tag%dt", idx);
  snprintf(keyBusy, sizeof(keyBusy), "tag%db", idx);

  prefs.begin("esl-dooray", false);
  prefs.putString(key, TAGS[idx].lastHash);
  prefs.putULong(keyTime, TAGS[idx].lastUpdate);
  prefs.putBool(keyBusy, TAGS[idx].lastBusyState);
  prefs.end();
}

static void loadTagStates() {
  prefs.begin("esl-dooray", true);
  for (int i=0;i<NUM_TAGS;i++) {
    char key[16], keyTime[16], keyBusy[16];
    snprintf(key,     sizeof(key),     "tag%d",  i);
    snprintf(keyTime, sizeof(keyTime), "tag%dt", i);
    snprintf(keyBusy, sizeof(keyBusy), "tag%db", i);

    String hash = prefs.getString(key, "");
    // 안전 복사
    strlcpy(TAGS[i].lastHash, hash.c_str(), sizeof(TAGS[i].lastHash));
    TAGS[i].lastUpdate   = prefs.getULong(keyTime, 0);
    TAGS[i].lastBusyState= prefs.getBool(keyBusy, false);
  }
  prefs.end();
}

// ------------------- Time -------------------
static inline void setTZ_KST(){ setenv("TZ","KST-9",1); tzset(); }
static inline bool timeReady(){ return time(nullptr) > 1700000000; }

static void ensureTime(){
  if (timeReady()){ setTZ_KST(); return; }
  setTZ_KST();
  configTzTime("KST-9","kr.pool.ntp.org","time.google.com");
  struct tm ti;
  for(int i=0;i<20;i++){
    if (getLocalTime(&ti,500)){ setTZ_KST(); return; }
    delay(100);
  }
}

static void setupSNTP() {
  sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
  sntp_set_sync_interval(6UL * 60UL * 60UL * 1000UL); // 6시간 재동기화
}

// ------------------- Utils -------------------
static void urlEncode(const char* s, char* out, size_t maxLen){
  const char* hex="0123456789ABCDEF";
  size_t j = 0;
  for (size_t i=0; s[i] && j<maxLen-4; ++i){
    unsigned char c=(unsigned char)s[i];
    if (isalnum(c) || c=='-'||c=='_'||c=='.'||c=='~'){
      out[j++] = c;
    } else {
      out[j++] = '%';
      out[j++] = hex[(c>>4)&0xF];
      out[j++] = hex[c&0xF];
    }
  }
  out[j] = '\0';
}

// ISO8601 파서: 'Z' 또는 ±hh:mm 오프셋 보정 (KST 기준)
static time_t parseISO(const char* iso, uint8_t& h, uint8_t& m){
  if(!iso || strlen(iso) < 19) return 0;

  int Y,Mo,D,Ho,Mi,Se;
  if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &Y,&Mo,&D,&Ho,&Mi,&Se) != 6) return 0;

  int sign = 0, offH = 0, offM = 0;
  const char* tzp = iso + 19; // 'Z' or +hh:mm/-hh:mm
  if (*tzp == 'Z') { sign = 0; }
  else if (*tzp == '+' || *tzp == '-') {
    sign = (*tzp == '+') ? +1 : -1;
    if (sscanf(tzp+1, "%2d:%2d", &offH, &offM) != 2) { offH = offM = 0; sign = 0; }
  }

  struct tm tmv = {};
  tmv.tm_year = Y - 1900;
  tmv.tm_mon  = Mo - 1;
  tmv.tm_mday = D;
  tmv.tm_hour = Ho;
  tmv.tm_min  = Mi;
  tmv.tm_sec  = Se;

  time_t t = mktime(&tmv); // 현 TZ(KST) 기준

  if (*tzp == 'Z' || sign != 0) {
    int totalMin = 9*60 - (sign * (offH*60 + offM)); // KST(+9) - 입력 오프셋
    t += totalMin * 60;
  }

  struct tm lt; localtime_r(&t, &lt);
  h = lt.tm_hour; m = lt.tm_min;
  return t;
}

static void kstTodayISO(char* tMin, char* tMax){
  time_t now=time(nullptr);
  struct tm lt;
  localtime_r(&now,&lt);
  lt.tm_hour=0; lt.tm_min=0; lt.tm_sec=0;
  strftime(tMin, 40, "%Y-%m-%dT%H:%M:%S+09:00", &lt);

  time_t end=mktime(&lt)+24*3600;
  localtime_r(&end,&lt);
  strftime(tMax, 40, "%Y-%m-%dT%H:%M:%S+09:00", &lt);
}

// ------------------- Hash 생성 -------------------
static void generateDataHash(const RoomData& rd, char* hash) {
  MD5Builder md5;
  md5.begin();

  char buffer[256];
  for(int i=0;i<rd.count;i++){
    snprintf(buffer, sizeof(buffer), "%02d%02d%02d%02d%s%s",
             rd.list[i].startH, rd.list[i].startM,
             rd.list[i].endH, rd.list[i].endM,
             rd.list[i].who, rd.list[i].subj);
    md5.add((uint8_t*)buffer, strlen(buffer));
  }
  md5.calculate();
  strlcpy(hash, md5.toString().c_str(), 33);
}

// ------------------- Dooray fetch -------------------
static bool fetchReservationsToday(const char* resourceId, RoomData& rd){
  rd.count = 0; rd.ok = false;

  char tMin[40], tMax[40];
  kstTodayISO(tMin, tMax);

  char encodedId[128], encodedMin[128], encodedMax[128];
  urlEncode(resourceId, encodedId, sizeof(encodedId));
  urlEncode(tMin, encodedMin, sizeof(encodedMin));
  urlEncode(tMax, encodedMax, sizeof(encodedMax));

  char url[512];
  snprintf(url, sizeof(url), "%s/reservation/v1/resource-reservations?resourceIds=%s&timeMin=%s&timeMax=%s",
           DOORAY_BASE, encodedId, encodedMin, encodedMax);

  WiFiClientSecure cli;
  cli.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(12000);
  http.setReuse(false);

  char authHdr[256];
  snprintf(authHdr, sizeof(authHdr), "dooray-api %s", DOORAY_TOKEN);
  http.addHeader("Authorization", authHdr);
  http.addHeader("Accept","application/json");
  http.addHeader("Connection","close");

  if (!http.begin(cli, url)){
    Serial.println("[Dooray] http.begin fail");
    return false;
  }

  int code = http.GET();
  String body = (code == 200) ? http.getString() : "";
  http.end();

  Serial.printf("[Dooray] HTTP %d, %d bytes\n", code, body.length());
  if (code != 200 || body.length()==0) return false;

  // 필요한 필드만 필터
  StaticJsonDocument<512> filter;
  filter["result"][0]["subject"] = true;
  filter["result"][0]["startedAt"] = true;
  filter["result"][0]["endedAt"] = true;
  filter["result"][0]["users"]["from"]["member"]["name"] = true;
  filter["result"][0]["users"]["from"]["emailUser"]["name"] = true;

  DynamicJsonDocument doc(12288); // 여유 확대
  DeserializationError e = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (e){
    Serial.printf("[JSON] Error: %s\n", e.c_str());
    return false;
  }

  JsonArray arr = doc["result"].as<JsonArray>();
  if (arr.isNull()) return false;

  for (JsonVariant v : arr){
    if (rd.count >= 12) break;

    Resv& r = rd.list[rd.count];

    const char* subj = v["subject"] | "(제목 없음)";
    strlcpy(r.subj, subj, sizeof(r.subj));

    const char* start = v["startedAt"] | "";
    const char* end   = v["endedAt"]   | v["EndedAt"] | "";

    r.st = parseISO(start, r.startH, r.startM);
    r.ed = parseISO(end  , r.endH, r.endM);

    const char* who = v["users"]["from"]["member"]["name"] |
                      v["users"]["from"]["emailUser"]["name"] | "예약자";
    strlcpy(r.who, who, sizeof(r.who));

    if (r.st > 0 && r.ed > r.st) rd.count++;
  }

  std::sort(rd.list, rd.list + rd.count, [](const Resv&a, const Resv&b){
    return a.st < b.st;
  });

  rd.ok = true;
  generateDataHash(rd, rd.dataHash);
  return true;
}

static int getCurrentMeetingIdx(const RoomData& rd){
  if (!timeReady()) return -1;
  time_t now = time(nullptr);
  for(int i=0;i<rd.count;i++){
    if (now >= rd.list[i].st && now < rd.list[i].ed) return i;
  }
  return -1;
}

// ------------------- 화면 렌더링 -------------------
static void drawScreen(const Tag& tag, const RoomData& rd){
  if(!canvasBW || !canvasRED) return;

  canvasBW->fillScreen(1);  // white
  canvasRED->fillScreen(1); // white

  u8g2.setFont(u8g2_font_unifont_t_korean2);
  u8g2.setFontMode(1);

  const int topY=20, marginL=8, marginR=8;

  // 1) 날짜/시간 (정시 ':' / 30분 '.')
  u8g2.begin(*canvasBW);
  u8g2.setForegroundColor(0);
  u8g2.setBackgroundColor(1);

  char dateStr[32];
  if (timeReady()){
    time_t now=time(nullptr);
    struct tm lt; localtime_r(&now,&lt);
    const char* weekdays[]={"일","월","화","수","목","금","토"};
    char mark = (lt.tm_min==0) ? ':' : (lt.tm_min==30 ? '.' : ' ');
    snprintf(dateStr, sizeof(dateStr), "%d.%d(%s) %02d%c",
             lt.tm_mon+1, lt.tm_mday, weekdays[lt.tm_wday], lt.tm_hour, mark);
  } else {
    strcpy(dateStr, "--.--(-) --");
  }
  u8g2.drawUTF8(marginL, topY, dateStr);

  // 2) 회의실명 (중앙)
  int labelWidth = u8g2.getUTF8Width(tag.label);
  int labelX = (DRAW_W - labelWidth)/2;
  u8g2.drawUTF8(labelX, topY, tag.label);

  // 3) 상태표시 (우측)
  int currentIdx = getCurrentMeetingIdx(rd);
  bool busy = (currentIdx >= 0);
  const char* statusText = busy ? "회의중" : "예약가능";

  if (busy){
    u8g2.begin(*canvasRED);
    u8g2.setForegroundColor(0);
    int sw = u8g2.getUTF8Width(statusText);
    int bx = DRAW_W - marginR - sw - 14;
    int by = 6;
    canvasRED->fillRect(bx, by, sw+14, 18, 0);
    u8g2.drawUTF8(bx+7, topY, statusText);
  } else {
    u8g2.begin(*canvasBW);
    u8g2.setForegroundColor(0);
    int sw = u8g2.getUTF8Width(statusText);
    int bx = DRAW_W - marginR - sw - 14;
    // 테두리 없이 텍스트만
    u8g2.drawUTF8(bx+7, topY, statusText);
  }

  // 구분선
  canvasBW->drawFastHLine(0,30,DRAW_W,0);
  canvasBW->drawFastHLine(0,31,DRAW_W,0);

  // 예약 리스트
  const int y0=48, lineH=16, listX=6;

  if (rd.count==0){
    u8g2.begin(*canvasBW);
    u8g2.setForegroundColor(0);
    const char* msg="오늘 예약 없음";
    int msgW=u8g2.getUTF8Width(msg);
    u8g2.drawUTF8((DRAW_W-msgW)/2, 75, msg);

    const char* sub="예약 가능합니다";
    int subW=u8g2.getUTF8Width(sub);
    u8g2.drawUTF8((DRAW_W-subW)/2, 95, sub);
  } else {
    int shown = (rd.count>6)?6:rd.count;
    for (int i=0;i<shown;i++){
      int y=y0+i*lineH;
      bool isNow=(i==currentIdx);

      char timeStr[14];
      snprintf(timeStr,sizeof(timeStr),"%02d:%02d-%02d:%02d",
               rd.list[i].startH,rd.list[i].startM,
               rd.list[i].endH,rd.list[i].endM);

      char who[36];  strlcpy(who, rd.list[i].who, sizeof(who));
      if (strlen(who)>24){ who[22]='.'; who[23]='.'; who[24]='\0'; }

      char subj[80]; strlcpy(subj, rd.list[i].subj, sizeof(subj));
      if (strlen(subj)>42){ subj[40]='.'; subj[41]='.'; subj[42]='\0'; }

      if (isNow){
        u8g2.begin(*canvasRED);
        u8g2.setForegroundColor(0);
        u8g2.drawUTF8(listX+2, y, timeStr);

        u8g2.begin(*canvasBW);
        u8g2.setForegroundColor(0);
        char rest[128]; snprintf(rest,sizeof(rest)," %s/%s", who, subj);
        int tw=u8g2.getUTF8Width(timeStr);
        u8g2.drawUTF8(listX+2+tw, y, rest);
      } else {
        u8g2.begin(*canvasBW);
        u8g2.setForegroundColor(0);
        char full[150]; snprintf(full,sizeof(full),"%s %s/%s", timeStr, who, subj);
        u8g2.drawUTF8(listX+2, y, full);
      }
    }

    if (rd.count>6){
      u8g2.begin(*canvasBW);
      u8g2.setForegroundColor(0);
      char more[16]; snprintf(more,sizeof(more),"... 외 %d건", rd.count-6);
      u8g2.drawUTF8(listX+2, y0+6*lineH, more);
    }
  }

  if (!rd.ok){
    u8g2.begin(*canvasBW);
    u8g2.setForegroundColor(0);
    u8g2.drawUTF8(8,115,"※ 네트워크 오류");
  }

  canvasBW->drawRect(0,0,DRAW_W,DRAW_H,0);
}

// ------------------- Build payload -------------------
static std::vector<uint8_t> g_payload;

static void buildPayload(){
  if(!canvasBW || !canvasRED) return;

  g_payload.clear();
  g_payload.reserve(9472);

  const uint8_t* bw  = canvasBW->getBuffer();
  const uint8_t* red = canvasRED->getBuffer();
  const int bpr = (DRAW_W+7)/8;

  auto getBit=[&](const uint8_t* buf,int x,int y)->bool{
    int idx=y*bpr + (x>>3);
    uint8_t mask=(0x80>>(x&7));
    return (buf[idx]&mask)!=0;
  };

  auto pack=[&](const uint8_t* buf,bool ONE_IS_WHITE){
    for(int xx=0; xx<DRAW_W; ++xx){
      int x = MIRROR_X ? (DRAW_W-1-xx) : xx;
      for(int byteIdx=0; byteIdx<16; ++byteIdx){
        uint8_t b=0;
        for(int bit=0; bit<8; ++bit){
          int y=byteIdx*8+bit; if (y>=DRAW_H) continue;
          bool one=getBit(buf,x,y);
          uint8_t eslBit = ONE_IS_WHITE ? (one?1:0) : (one?0:1);
          if (eslBit) b |= (1<<(7-bit));
        }
        g_payload.push_back(b);
      }
    }
  };

  pack(bw , BW_ONE_IS_WHITE);
  pack(red, RED_ONE_IS_WHITE);
  Serial.printf("[PAYLOAD] %u bytes\n",(unsigned)g_payload.size());
}

// ------------------- BLE uploader -------------------
static BLEUUID SVC_UUID((uint16_t)0xFEF0);
static BLEUUID CMD_UUID((uint16_t)0xFEF1);
static BLEUUID IMG_UUID((uint16_t)0xFEF2);

static BLEClient* g_cli=nullptr;
static BLERemoteCharacteristic* g_cmd=nullptr;
static BLERemoteCharacteristic* g_img=nullptr;

static volatile bool g_got01=false, g_got02=false, g_got05=false;
static volatile uint8_t g_st05=0;
static volatile bool g_hasAck=false;
static volatile uint32_t g_ack=0;
static uint16_t g_partMsgSize=244;
static size_t   g_partDataSize=240;
static std::vector<uint8_t> g_lastChunk;
static uint32_t g_lastTxMs=0;

static void onNotifyCMD(BLERemoteCharacteristic*, uint8_t* p, size_t n, bool){
  if (!p || !n) return;
  uint8_t op=p[0];
  if (op==0x01 && n>=3){
    g_partMsgSize = (uint16_t)(p[1] | (p[2]<<8));
    g_partDataSize = (g_partMsgSize>=4)?(g_partMsgSize-4):240;
    g_got01=true;
  } else if (op==0x02){
    g_got02=true;
  } else if (op==0x05){
    g_got05=true;
    g_st05=(n>=2)?p[1]:0xFF;
    g_hasAck=(n>=6);
    if (g_hasAck) g_ack=(uint32_t)p[2]|((uint32_t)p[3]<<8)|((uint32_t)p[4]<<16)|((uint32_t)p[5]<<24);
  }
}

static void enableCCCDNotify(BLERemoteCharacteristic* c){
  auto d=c->getDescriptor(BLEUUID((uint16_t)0x2902));
  if(!d) return;
  uint8_t v[2]={0x01,0x00};
  d->writeValue(v,2,true);
  delay(100);
}

static bool connectAndDiscover(const Tag& t){
  if(g_cli){
    if(g_cli->isConnected()) g_cli->disconnect();
    delete g_cli; g_cli=nullptr;
    delay(500);
  }

  BLEAddress addr(t.mac);
  g_cli = BLEDevice::createClient();

  bool connected=false;
  for(int retry=0; retry<3; retry++){
    Serial.printf("[BLE] Try %d/3\n", retry+1);
    if (g_cli->connect(addr)){ connected=true; break; }
    delay(1000); esp_task_wdt_reset();
  }
  if(!connected){ Serial.println("[BLE] Failed"); return false; }

  delay(500);
  auto svc=g_cli->getService(SVC_UUID);
  if(!svc) return false;

  g_cmd=svc->getCharacteristic(CMD_UUID);
  g_img=svc->getCharacteristic(IMG_UUID);
  if(!g_cmd||!g_img) return false;

  g_cmd->registerForNotify(onNotifyCMD);
  enableCCCDNotify(g_cmd);
  delay(200);
  return true;
}

static void disconnectAndCleanup(){
  if(g_cli){
    if(g_cli->isConnected()) g_cli->disconnect();
    delete g_cli; g_cli=nullptr;
  }
  g_cmd=nullptr; g_img=nullptr;
  delay(500);
}

static void le32(uint8_t* p, uint32_t v){ p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }

static void sendPartA(uint32_t part){
  size_t total=g_payload.size();
  size_t dataSz=std::min((size_t)g_partDataSize,(size_t)240);
  size_t off=part*dataSz;
  if(off>=total) return;

  size_t n=std::min(dataSz,total-off);
  g_lastChunk.resize(4+n);
  le32(g_lastChunk.data(),part);
  memcpy(&g_lastChunk[4],&g_payload[off],n);

  if(g_img) g_img->writeValue(g_lastChunk.data(), g_lastChunk.size(), false);
  g_lastTxMs=millis();

  if((part%20)==0){
    uint32_t parts=(total+dataSz-1)/dataSz;
    Serial.printf("[TX] %u/%u\n",part,parts);
  }
}

static bool tryProtocolA(){
  g_got01=g_got02=g_got05=false; g_hasAck=false;
  g_partMsgSize=244; g_partDataSize=240; g_lastChunk.clear();

  uint8_t c1=0x01;
  if(g_cmd) g_cmd->writeValue(&c1,1,true);

  uint32_t t0=millis();
  while(millis()-t0<3000){
    if(g_got01) break;
    if(!g_cli->isConnected()) return false;
    delay(10); esp_task_wdt_reset();
  }
  if(!g_got01) return false;

  uint32_t total=g_payload.size();
  uint8_t c2[8]={0x02,(uint8_t)(total&0xFF),(uint8_t)((total>>8)&0xFF),
                 (uint8_t)((total>>16)&0xFF),(uint8_t)((total>>24)&0xFF),0,0,0};
  if(g_cmd) g_cmd->writeValue(c2,sizeof(c2),false);

  delay(100);
  uint8_t c3=0x03;
  if(g_cmd) g_cmd->writeValue(&c3,1,false);

  size_t dataSz=std::min((size_t)g_partDataSize,(size_t)240);
  uint32_t parts=(g_payload.size()+dataSz-1)/dataSz;
  sendPartA(0);

  uint32_t lastAckPart=UINT32_MAX;
  uint32_t stallCount=0;

  while(true){
    if(!g_cli->isConnected()) return false;

    if(g_got05){
      g_got05=false; stallCount=0;
      if(g_st05==0x08) return true;
      else if(g_st05!=0x00) return false;
      else if(g_hasAck){
        uint32_t ack=g_ack;
        if(ack>=parts) return true;
        if(ack!=lastAckPart){ lastAckPart=ack; sendPartA(ack); }
      }
    }

    if(!g_lastChunk.empty() && millis()-g_lastTxMs>1500){
      if(g_img) g_img->writeValue(g_lastChunk.data(), g_lastChunk.size(), false);
      g_lastTxMs=millis(); stallCount++;
      if(stallCount>20) return false;
    }

    if(millis()-t0>60000) return false;
    delay(5); esp_task_wdt_reset();
  }
}

// ------------------- Wi-Fi -------------------
static void wifiOnAndConnect(){
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t0=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-t0<20000){
    delay(200); esp_task_wdt_reset();
  }
  if(WiFi.status()==WL_CONNECTED){
    Serial.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());
  }
}

static void wifiOff(){
  WiFi.disconnect(true, true);
  delay(300);
  esp_wifi_stop();
  delay(200);
  esp_err_t e=esp_wifi_deinit();
  (void)e; // 상태 레이스 무시
  delay(200);
  WiFi.mode(WIFI_OFF);
  delay(500);
}

// ------------------- 업데이트 루틴 -------------------
static void processUpdates(bool forceUpdate){
  Serial.println("\n═══ Update Process ═══");

  wifiOnAndConnect();
  if(WiFi.status()==WL_CONNECTED){
    ensureTime();
  } else {
    Serial.println("[ERROR] WiFi failed (will still refresh screen)");
  }

  uint32_t nowTs=(uint32_t)time(nullptr);
  int updateCount=0;

  for(int i=0;i<NUM_TAGS;i++){
    Serial.printf("[%d/%d] %s... ", i+1, NUM_TAGS, TAGS[i].label);

    bool fetchOk=false;
    if(WiFi.status()==WL_CONNECTED){
      fetchOk=fetchReservationsToday(TAGS[i].resId, g_room[i]);
    } else {
      g_room[i].count=0; g_room[i].ok=false; g_room[i].dataHash[0]='\0';
    }

    if(!fetchOk) Serial.println("FAIL");
    else         Serial.println("OK");

    bool isBusy       = (getCurrentMeetingIdx(g_room[i]) >= 0);
    bool dataChanged  = fetchOk ? (strcmp(g_room[i].dataHash, TAGS[i].lastHash)!=0) : false;
    bool statusChanged= (TAGS[i].lastBusyState != isBusy);
    bool timeExpired  = (nowTs - TAGS[i].lastUpdate) >= (30*60);

    if (forceUpdate || dataChanged || statusChanged || timeExpired || !fetchOk){
      TAGS[i].needsUpdate=true;
      TAGS[i].lastBusyState=isBusy;
      updateCount++;
      Serial.printf("UPDATE (data:%d, status:%d, fetch:%d)\n", dataChanged, statusChanged, fetchOk);
    } else {
      TAGS[i].needsUpdate=false;
      Serial.println("SKIP");
    }

    esp_task_wdt_reset();
  }

  wifiOff();
  delay(1000);

  if(updateCount>0){
    Serial.printf("\n[INFO] Updating %d tags\n", updateCount);
    BLEDevice::init("ESL");
    delay(800);

    for(int i=0;i<NUM_TAGS;i++){
      if(!TAGS[i].needsUpdate) continue;

      Serial.printf("\n[Update] %s... ", TAGS[i].label);
      drawScreen(TAGS[i], g_room[i]);
      buildPayload();

      bool uploadOk=false;
      for(int attempt=0; attempt<3 && !uploadOk; ++attempt){
        if(!connectAndDiscover(TAGS[i])){
          disconnectAndCleanup();
          delay(1200);
          continue;
        }
        uploadOk=tryProtocolA();
        disconnectAndCleanup();
        if(!uploadOk && attempt<2) delay(1800); // 백오프
      }

      if(uploadOk){
        if(g_room[i].ok){
          strlcpy(TAGS[i].lastHash, g_room[i].dataHash, sizeof(TAGS[i].lastHash));
        }
        TAGS[i].lastUpdate=nowTs;
        saveTagState(i);
        Serial.println("OK");
      } else {
        Serial.println("FAIL");
      }

      delay(1200); esp_task_wdt_reset();
    }

    BLEDevice::deinit();
  } else {
    Serial.println("\n[INFO] No updates needed");
  }

  Serial.printf("[MEM] Free: %d bytes\n", ESP.getFreeHeap());
}

// ------------------- 5분 폴링: 변경 감지 전용 -------------------
// Dooray를 가볍게 조회만 하여 변경이 있으면 true 리턴 (BLE 업로드는 하지 않음)
static bool pollDoorayForChanges(){
  bool changed=false;

  wifiOnAndConnect();
  if (WiFi.status()!=WL_CONNECTED){
    Serial.println("[Poll] WiFi fail");
    wifiOff();
    return false;
  }
  if (!timeReady()) ensureTime();

  for(int i=0;i<NUM_TAGS;i++){
    RoomData tmp{};
    bool ok=fetchReservationsToday(TAGS[i].resId, tmp);
    if (!ok){
      Serial.printf("[Poll] %s: fetch FAIL\n", TAGS[i].label);
      continue;
    }
    bool busyNow = (getCurrentMeetingIdx(tmp) >= 0);
    bool dataCh  = (strcmp(tmp.dataHash, TAGS[i].lastHash)!=0);
    bool statCh  = (TAGS[i].lastBusyState != busyNow);

    if (dataCh || statCh){
      Serial.printf("[Poll] %s: change detected (data:%d, status:%d)\n", TAGS[i].label, dataCh, statCh);
      changed=true;
      // 주의: 실제 lastHash/lastBusyState는 processUpdates 성공 시 갱신
    } else {
      Serial.printf("[Poll] %s: no change\n", TAGS[i].label);
    }
    esp_task_wdt_reset();
  }

  wifiOff();
  return changed;
}

// ------------------- Arduino -------------------
void setup(){
  Serial.begin(115200);
  delay(1500);

  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);

  if(esp_bt_controller_get_status()==ESP_BT_CONTROLLER_STATUS_IDLE){
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  }

  Serial.println("\n╔════════════════════════════════════╗");
  Serial.println("║   ESL Dooray Final v4.2 (Enhanced) ║");
  Serial.println("║   30분 강제 + 5분 폴링 변경 반영   ║");
  Serial.println("╚════════════════════════════════════╝");

  // 캔버스 1회 생성 (상주)
  if(!canvasBW)  canvasBW  = new GFXcanvas1(DRAW_W, DRAW_H);
  if(!canvasRED) canvasRED = new GFXcanvas1(DRAW_W, DRAW_H);
  u8g2.begin(*canvasBW);

  loadTagStates();
  setupSNTP();

  bool isFirstRun = (TAGS[0].lastUpdate == 0);
  processUpdates(isFirstRun); // 첫 실행 강제 업데이트
}

void loop(){
  delay(30000); // 30초 폴링

  if (!timeReady()){
    // 아직 시간 준비가 안되었으면 가끔 보정
    wifiOnAndConnect();
    ensureTime();
    wifiOff();
  }

  time_t now=time(nullptr);
  struct tm lt;
  localtime_r(&now,&lt);

  // === 30분 강제 업데이트 (정시/30분) ===
  if (lt.tm_min==0 || lt.tm_min==30){
    static int lastHour30=-1, lastMin30=-1;
    if (lt.tm_hour!=lastHour30 || lt.tm_min!=lastMin30){
      lastHour30=lt.tm_hour; lastMin30=lt.tm_min;
      Serial.printf("\n[TIME] %02d:%02d - Force 30-min update\n", lt.tm_hour, lt.tm_min);
      processUpdates(true);
      // 30분 강제 업데이트가 실행되었다면, 아래 5분 폴링은 굳이 중복 실행 안 해도 됨
      esp_task_wdt_reset();
      return;
    }
  }

  // === 5분 폴링 (변경 감지 시 즉시 업데이트) ===
  if ((lt.tm_min % 5)==0){
    static int lastMinPoll=-1, lastHourPoll=-1;
    if (lt.tm_min!=lastMinPoll || lt.tm_hour!=lastHourPoll){
      lastMinPoll = lt.tm_min; lastHourPoll = lt.tm_hour;
      Serial.printf("\n[TIME] %02d:%02d - 5-min poll\n", lt.tm_hour, lt.tm_min);
      bool changed = pollDoorayForChanges();
      if (changed){
        Serial.println("[Poll] Change detected → running immediate update");
        processUpdates(true); // 즉시 반영
      }
    }
  }

  esp_task_wdt_reset();
}
