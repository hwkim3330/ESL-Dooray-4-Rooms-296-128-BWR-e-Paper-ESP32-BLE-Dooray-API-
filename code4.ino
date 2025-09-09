#include <Arduino.h>
#include <vector>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <BLEAddress.h>

#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>

// ===== 사용자 설정 =====
static constexpr int W = 128;   // 296x128 BWR(0032) 기준: 가로 128, 세로 296
static constexpr int H = 296;
static constexpr bool MIRROR_X = false;
static constexpr bool INVERT   = false;

struct Target { const char* name; const char* mac; const char* label; };
Target TARGETS[] = {
  { "NEMR92957578", "ff:ff:92:95:75:78", "회의실 A" },
  { "NEMR92957306", "ff:ff:92:95:73:06", "회의실 B" },
  { "NEMR92957304", "ff:ff:92:95:73:04", "회의실 C" },
  { "NEMR92959581", "ff:ff:92:95:95:81", "회의실 D" },
};
static constexpr int WHICH = 0;  // 위 배열에서 선택

// ===== GATT UUID =====
static BLEUUID SVC_UUID((uint16_t)0xFEF0);
static BLEUUID CMD_UUID((uint16_t)0xFEF1);
static BLEUUID IMG_UUID((uint16_t)0xFEF2);

// ===== 전역 =====
BLEClient* g_cli = nullptr;
BLERemoteCharacteristic* g_cmd = nullptr; // notify + write
BLERemoteCharacteristic* g_img = nullptr; // write (이미지)

GFXcanvas1 canvas(W, H);
U8G2_FOR_ADAFRUIT_GFX u8g2;

std::vector<uint8_t> g_payload;
size_t   g_off = 0;
uint32_t g_partUpload = 0;
std::vector<uint8_t> g_lastChunk;

uint16_t g_partMsgSize = 244; // 전체 메시지 크기(앞 4B 포함)
size_t   g_partDataSize = 240; // 실제 전송 데이터 (= msg - 4)

// 상태/이벤트 플래그
enum State { ST_IDLE, ST_WAIT_01, ST_WAIT_02, ST_SENT_03, ST_SENDING, ST_DONE, ST_ERR };
State g_state = ST_IDLE;

volatile bool g_evt_got01 = false;
volatile bool g_evt_got02 = false;
volatile bool g_evt_got05 = false;
volatile uint8_t  g_evt05_status = 0;
volatile bool g_evt05_hasAck = false;
volatile uint32_t g_evt05_ackPart = 0;

uint32_t g_t_sent02 = 0;

// ===== 유틸 =====
static inline void le32(uint8_t* p, uint32_t v) {
  p[0]=uint8_t(v);
  p[1]=uint8_t(v>>8);
  p[2]=uint8_t(v>>16);
  p[3]=uint8_t(v>>24);
}

static inline uint8_t getPixelWhite(int x, int y) {
  if (MIRROR_X) x = (W - 1) - x;
  int bytes_per_row = (W + 7) / 8;
  int idx = (y * bytes_per_row) + (x >> 3);
  uint8_t bit = 0x80 >> (x & 7);
  bool on = (canvas.getBuffer()[idx] & bit) != 0; // GFXcanvas1: 1=픽셀 on(검)
  uint8_t white = on ? 0 : 1; // on이면 검정 → 흑백 바이트에서 0이 검, 1이 흰 기준으로 뒤집기
  if (INVERT) white = !white;
  return white;
}

// 텍스트 렌더
void renderText(const String& text) {
  canvas.fillScreen(1); // 흰
  u8g2.begin(canvas);
  u8g2.setFont(u8g2_font_unifont_t_korean2);
  u8g2.setFontMode(1);
  u8g2.setForegroundColor(0); // 검
  u8g2.setBackgroundColor(1); // 흰
  int16_t tw = u8g2.getUTF8Width(text.c_str());
  int16_t x  = (W - tw) / 2;
  int16_t y  = H / 2;
  u8g2.drawUTF8(x, y, text.c_str());
}

// Andrew 웹 방식 0x75 압축 페이로드 생성
void buildCompressedPayload(std::vector<uint8_t>& out) {
  out.clear();
  out.resize(4, 0x00);
  const int byte_per_line = H / 8;
  for (int x = 0; x < W; ++x) {
    out.push_back(0x75);
    out.push_back((uint8_t)(byte_per_line + 7));
    out.push_back((uint8_t)byte_per_line);
    out.push_back(0x00); out.push_back(0x00); out.push_back(0x00); out.push_back(0x00);
    for (int b = 0; b < byte_per_line; ++b) {
      uint8_t v = 0;
      for (int k = 0; k < 8; ++k) {
        int y = b*8 + k;
        uint8_t white = getPixelWhite(x, y);
        int bit = 7 - k;
        if (white) v |= (1 << bit);
      }
      out.push_back(v);
    }
  }
  uint32_t L = (uint32_t)out.size();
  out[0]=L&0xFF; out[1]=(L>>8)&0xFF; out[2]=(L>>16)&0xFF; out[3]=(L>>24)&0xFF;
}

// ===== 쓰기 헬퍼 (알림 콜백 외부에서만 호출) =====

// 0x01은 with-response(안전), 0x02/0x03은 without-response(교착 방지)
bool cmdWrite(const uint8_t* data, size_t len, bool withResp) {
  if (!g_cmd) return false;
  std::vector<uint8_t> tmp(data, data + len);
  g_cmd->writeValue(tmp.data(), tmp.size(), withResp);
  return true;
}

bool imgWrite(const uint8_t* data, size_t len) {
  if (!g_img) return false;
  std::vector<uint8_t> tmp(data, data + len);
  g_img->writeValue(tmp.data(), tmp.size(), false);
  return true;
}

// ===== 이미지 파트 전송 =====
void sendNextPart(uint32_t ackPart) {
  if (ackPart != g_partUpload) {
    // 재전송
    Serial.printf("[TX] 재전송 part=%u(ack=%u)\n", g_partUpload-1, ackPart);
    if (!g_lastChunk.empty()) imgWrite(g_lastChunk.data(), g_lastChunk.size());
    return;
  }

  if (g_off >= g_payload.size()) {
    Serial.println("[TX] 모든 파트 큐잉 완료(최종 0x05 0x08 대기)");
    return;
  }

  size_t remain = g_payload.size() - g_off;
  size_t n = (remain > g_partDataSize) ? g_partDataSize : remain;

  g_lastChunk.assign(4 + n, 0);
  le32(g_lastChunk.data(), g_partUpload);
  memcpy(&g_lastChunk[4], &g_payload[g_off], n);

  Serial.printf("[TX] part=%u bytes=%u\n", g_partUpload, (unsigned)n);
  imgWrite(g_lastChunk.data(), g_lastChunk.size());

  g_off += n;
  g_partUpload++;
}

// ===== 알림 콜백 (여기선 플래그만 세팅, 쓰기 금지) =====
static void notifyCB(BLERemoteCharacteristic* c, uint8_t* p, size_t len, bool) {
  if (!p || !len) return;
  Serial.print("[NTF] ");
  for (size_t i=0;i<len;++i){ char b[4]; sprintf(b,"%02X",p[i]); Serial.print(b);}
  Serial.println();

  uint8_t op = p[0];
  if (op == 0x01) {
    if (len >= 3) {
      uint16_t msg = (uint16_t)(p[1] | (p[2]<<8));
      g_partMsgSize = msg;
      g_partDataSize = (msg >= 4) ? (msg - 4) : 0;
      if (g_partDataSize == 0) g_partDataSize = 16; // 안전빵
      // 이벤트 세트
      g_evt_got01 = true;
    }
  } else if (op == 0x02) {
    g_evt_got02 = true;
  } else if (op == 0x05) {
    g_evt_got05 = true;
    g_evt05_status = (len >= 2) ? p[1] : 0xFF;
    g_evt05_hasAck = (len >= 6);
    if (g_evt05_hasAck) {
      g_evt05_ackPart = (uint32_t)p[2] | ((uint32_t)p[3]<<8) | ((uint32_t)p[4]<<16) | ((uint32_t)p[5]<<24);
    }
  }
}

// ===== CCCD =====
static bool enableNotifyCCCD(BLERemoteCharacteristic* ch) {
  if (!ch) return false;
  BLERemoteDescriptor* cccd = ch->getDescriptor(BLEUUID((uint16_t)0x2902));
  if (!cccd) { Serial.println("[WARN] CCCD(0x2902) 없음"); return false; }
  uint8_t v[2] = {0x01,0x00}; // Notify on
  cccd->writeValue(v, 2, true);
  Serial.println("[OK] CCCD notify enabled (0x01 0x00)");
  delay(120);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n-> === ESL (FEF0) Uploader: Meeting Room Name ===");

  // 이미지 생성
  String text = TARGETS[WHICH].label;
  canvas.fillScreen(1);
  u8g2.begin(canvas);
  u8g2.setFont(u8g2_font_unifont_t_korean2);
  u8g2.setFontMode(1);
  u8g2.setForegroundColor(0);
  u8g2.setBackgroundColor(1);
  {
    int16_t tw = u8g2.getUTF8Width(text.c_str());
    int16_t x  = (W - tw) / 2;
    int16_t y  = H / 2;
    u8g2.drawUTF8(x, y, text.c_str());
  }
  buildCompressedPayload(g_payload);
  Serial.printf("[IMG] %dx%d  payload=%u bytes\n", W, H, (unsigned)g_payload.size());

  // BLE 연결
  BLEDevice::init("");
  g_cli = BLEDevice::createClient();

  BLEAddress addr(TARGETS[WHICH].mac);
  Serial.printf("[CONNECT] %s (%s)\n", TARGETS[WHICH].name, addr.toString().c_str());
  if (!g_cli->connect(addr)) { Serial.println("[ERR] connect fail"); return; }
  Serial.println("[OK] connected");

  BLERemoteService* svc = g_cli->getService(SVC_UUID);
  if (!svc) { Serial.println("[ERR] FEF0 서비스 없음"); g_cli->disconnect(); return; }
  g_img = svc->getCharacteristic(IMG_UUID);
  g_cmd = svc->getCharacteristic(CMD_UUID);
  if (!g_img || !g_cmd) { Serial.println("[ERR] FEF1/FEF2 특성 없음"); g_cli->disconnect(); return; }

  g_cmd->registerForNotify(notifyCB);
  enableNotifyCCCD(g_cmd);
  delay(100);

  // 초기화
  g_state = ST_WAIT_01;
  g_evt_got01 = g_evt_got02 = g_evt_got05 = false;
  g_evt05_hasAck = false;
  g_partUpload = 0;
  g_off = 0;
  g_lastChunk.clear();

  // 0x01 전송 (with-response)
  {
    uint8_t start = 0x01;
    Serial.println("[STEP] Send 0x01");
    cmdWrite(&start, 1, true);
  }
}

void loop() {
  if (!g_cli || !g_cli->isConnected()) {
    if (g_state != ST_DONE && g_state != ST_ERR)
      Serial.println("[ERR] 연결 끊김");
    delay(500);
    return;
  }

  switch (g_state) {
    case ST_WAIT_01:
      if (g_evt_got01) {
        g_evt_got01 = false;
        Serial.printf("[STEP] Image part message size=%u -> data size=%u\n",
                      g_partMsgSize, (unsigned)g_partDataSize);

        // 0x02 + [총바이트(LE 4B)] + 000000(3B) — write without response
        {
          uint32_t totalBytes = (uint32_t)g_payload.size();
          uint8_t cmd[8] = {0x02, 0,0,0,0, 0x00,0x00,0x00};
          le32(&cmd[1], totalBytes);
          Serial.printf("[STEP] Send 0x02, total=%u\n", (unsigned)totalBytes);
          cmdWrite(cmd, sizeof(cmd), false); // ⚠ 콜백 밖 + without-response
        }
        g_t_sent02 = millis();
        g_state = ST_WAIT_02;
      }
      break;

    case ST_WAIT_02:
      if (g_evt_got02) {
        g_evt_got02 = false;
        // 0x03 — write without response
        uint8_t c = 0x03;
        Serial.println("[STEP] Send 0x03");
        cmdWrite(&c, 1, false);
        g_state = ST_SENT_03;
      } else if (millis() - g_t_sent02 > 800) {
        // 폴백: 0x02 응답이 없어도 0x03 강행
        uint8_t c = 0x03;
        Serial.println("[FALLBACK] No 0x02 notify. Sending 0x03 proactively.");
        cmdWrite(&c, 1, false);
        g_state = ST_SENT_03;
      }
      break;

    case ST_SENT_03:
      // 첫 part 송신 시작을 위해, 가짜 ack=현재 part번호로 호출
      sendNextPart(g_partUpload);
      g_state = ST_SENDING;
      break;

    case ST_SENDING:
      if (g_evt_got05) {
        g_evt_got05 = false;
        if (g_evt05_status == 0x08) {
          Serial.println("[DONE] 이미지 업로드 완료");
          g_state = ST_DONE;
        } else if (g_evt05_status != 0x00) {
          Serial.printf("[ERR] status=0x%02X\n", g_evt05_status);
          g_state = ST_ERR;
        } else {
          if (g_evt05_hasAck) {
            sendNextPart(g_evt05_ackPart);
          }
        }
      }
      break;

    case ST_DONE:
      // 필요하면 리프레시/재연결 루틴 추가
      delay(1000);
      return;

    case ST_ERR:
      Serial.println("[ERR] 업로드 실패");
      delay(1000);
      return;

    default: break;
  }

  delay(10);
}




업로드는 성공한코드
