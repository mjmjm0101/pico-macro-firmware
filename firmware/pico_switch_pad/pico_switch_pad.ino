#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include "switch_tinyusb.h"
#include "version.h"

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

// ==============================
// Wi-Fi config struct + globals
// ==============================
struct WifiConfig {
  String ssid;
  String password;
  IPAddress localIP;
  String port;
  IPAddress gateway;
  IPAddress subnet;
  bool valid;
};

WifiConfig gWifiConfig = { "", "", IPAddress(), "", IPAddress(), IPAddress(), false };
bool wifiStarted = false;
bool fsMounted   = false;

WiFiServer* gServer = nullptr;
uint16_t gTcpPort   = 5000;

WiFiClient gCurrentClient;
String     gWifiLineBuffer = "";

// ==============================
// FS helper: mount LittleFS
// ==============================
bool ensureFS() {
  if (fsMounted) return true;

  if (LittleFS.begin()) {
    Serial.println("[FS] LittleFS mounted");
    fsMounted = true;
    return true;
  }

  Serial.println("[FS] LittleFS mount failed, retrying once...");
  delay(50);

  if (LittleFS.begin()) {
    Serial.println("[FS] LittleFS mounted on second try");
    fsMounted = true;
    return true;
  }

  Serial.println("[FS] still failed, trying format...");

  if (!LittleFS.format()) {
    Serial.println("[FS] format failed");
    return false;
  }

  if (!LittleFS.begin()) {
    Serial.println("[FS] mount after format failed");
    return false;
  }

  Serial.println("[FS] formatted and mounted");
  fsMounted = true;
  return true;
}

// ==============================
// Load config from /wifi.cfg
//   1: ssid
//   2: password
//   3: ip
//   4: port
//   5: gateway
//   6: subnet
// ==============================
bool loadWifiConfig() {
  if (!ensureFS()) {
    Serial.println("[CFG] loadWifiConfig: FS not mounted");
    return false;
  }

  File f = LittleFS.open("/wifi.cfg", "r");
  if (!f) {
    Serial.println("[CFG] wifi.cfg not found -> Wi-Fi config missing");
    return false;
  }

  String ssid = f.readStringUntil('\n'); ssid.trim();
  String pass = f.readStringUntil('\n'); pass.trim();
  String ip   = f.readStringUntil('\n'); ip.trim();
  String port = f.readStringUntil('\n'); port.trim();
  String gw   = f.readStringUntil('\n'); gw.trim();
  String sn   = f.readStringUntil('\n'); sn.trim();
  f.close();

  IPAddress ipAddr, gwAddr, snAddr;
  if (!ipAddr.fromString(ip) || !gwAddr.fromString(gw) || !snAddr.fromString(sn)) {
    Serial.println("[CFG] IP/GW/SN parse failed -> Wi-Fi config invalid");
    return false;
  }

  if (ssid.length() == 0) {
    Serial.println("[CFG] SSID empty -> Wi-Fi config invalid");
    return false;
  }

  if (port.length() == 0) {
    port = "5000";
  }

  gWifiConfig.ssid     = ssid;
  gWifiConfig.password = pass;
  gWifiConfig.port     = port;
  gWifiConfig.localIP  = ipAddr;
  gWifiConfig.gateway  = gwAddr;
  gWifiConfig.subnet   = snAddr;
  gWifiConfig.valid    = true;

  Serial.println("[CFG] wifi.cfg loaded OK");
  Serial.print("[CFG] SSID: "); Serial.println(gWifiConfig.ssid);
  Serial.print("[CFG] IP  : "); Serial.println(gWifiConfig.localIP);
  Serial.print("[CFG] PORT: "); Serial.println(gWifiConfig.port);
  Serial.print("[CFG] GW  : "); Serial.println(gWifiConfig.gateway);
  Serial.print("[CFG] SN  : "); Serial.println(gWifiConfig.subnet);

  return true;
}

// ==============================
// Save config to /wifi.cfg
// ==============================
bool saveWifiConfig(const WifiConfig &cfg) {
  if (!ensureFS()) {
    Serial.println("[CFG] saveWifiConfig: FS not mounted");
    return false;
  }

  File f = LittleFS.open("/wifi.cfg", "w");
  if (!f) {
    Serial.println("[CFG] open(/wifi.cfg, w) failed");
    return false;
  }

  f.println(cfg.ssid);
  f.println(cfg.password);
  f.println(cfg.localIP.toString());
  f.println(cfg.port);
  f.println(cfg.gateway.toString());
  f.println(cfg.subnet.toString());
  f.close();

  Serial.println("[CFG] wifi.cfg saved");
  return true;
}

// ==============================
// Wi-Fi start/stop helpers
// ==============================
void stopWifiServer() {
  Serial.println("[WiFi] Stopping Wi-Fi server...");

  if (gServer) {
    gServer->stop();
    delete gServer;
    gServer = nullptr;
  }

  if (gCurrentClient) {
    gCurrentClient.stop();
  }
  gWifiLineBuffer = "";

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  wifiStarted = false;

  Serial.println("[WiFi] Stopped.");
}

bool startWifiFromConfig() {
  if (!gWifiConfig.valid) {
    Serial.println("[WiFi] startWifiFromConfig: config invalid");
    return false;
  }

  Serial.println("[WiFi] config found -> attempting to connect");
  WiFi.mode(WIFI_STA);
  WiFi.config(gWifiConfig.localIP, gWifiConfig.gateway, gWifiConfig.subnet);

  Serial.print("[WiFi] Connecting to SSID: ");
  Serial.println(gWifiConfig.ssid);

  WiFi.begin(gWifiConfig.ssid.c_str(), gWifiConfig.password.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print("status = ");
    Serial.println(WiFi.status());
    delay(200);
    yield();

    if (millis() - start > 15000) {
      Serial.println("[WiFi] TIMEOUT! Could not connect.");
      break;
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] connect failed");
    wifiStarted = false;
    return false;
  }

  uint16_t portNum = (uint16_t)gWifiConfig.port.toInt();
  if (portNum == 0) {
    portNum = 5000;
  }
  gTcpPort = portNum;

  if (gServer) {
    gServer->stop();
    delete gServer;
    gServer = nullptr;
  }

  gServer = new WiFiServer(gTcpPort);
  gServer->begin();

  Serial.println();
  Serial.print("[WiFi] Connected! IP = ");
  Serial.print(WiFi.localIP());
  Serial.print(", Port = ");
  Serial.println(gTcpPort);

  wifiStarted = true;
  return true;
}

// ==============================
// Serial config protocol
// ==============================
bool cfgReceiving = false;
WifiConfig incomingCfg;
String serialLine = "";

// forward
void handleCommand(const String& rawLine, WiFiClient *client = nullptr);
void handleConfigCommand(const String &line);

// ==============================
// Serial input task
// ==============================
void processSerialInput() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      String line = serialLine;
      line.trim();
      if (line.length()) {
        Serial.print("[CFG] line: ");
        Serial.println(line);
        handleConfigCommand(line);
      }
      serialLine = "";
    } else if (c != '\r') {
      serialLine += c;
    }
    yield();
  }
}

void resetIncomingCfg() {
  incomingCfg.ssid     = "";
  incomingCfg.password = "";
  incomingCfg.localIP  = IPAddress();
  incomingCfg.port     = "";
  incomingCfg.gateway  = IPAddress();
  incomingCfg.subnet   = IPAddress();
  incomingCfg.valid    = false;
}

void handleConfigCommand(const String &line) {
  if (line == "VERSION") {
    Serial.print("[FW] VERSION ");
    Serial.println(FW_VERSION);
    return;
  }

  // ---- FS commands ----
  if (line == "FS FORMAT") {
    Serial.println("[FS] FORMAT requested...");
    LittleFS.end();
    fsMounted = false;

    if (!LittleFS.format()) {
      Serial.println("[FS] format FAIL");
      return;
    }

    if (ensureFS()) {
      Serial.println("[FS] format success and mounted");
    } else {
      Serial.println("[FS] format success but mount FAIL");
    }
    return;
  }

  if (line == "FS INFO") {
    Serial.println("[FS] INFO requested");
    if (!ensureFS()) {
      Serial.println("[FS] ensureFS() failed -> cannot mount");
      return;
    }

    Serial.println("[FS] FS is mounted");
    if (LittleFS.exists("/wifi.cfg")) {
      Serial.println("[FS] wifi.cfg exists");
    } else {
      Serial.println("[FS] wifi.cfg missing");
    }
    return;
  }

  if (line == "FS TEST") {
    Serial.println("[FS] TEST begin");
    if (!ensureFS()) {
      Serial.println("[FS] ensureFS() failed -> mount FAIL");
      return;
    }

    File test = LittleFS.open("/test.txt","w");
    if (!test) {
      Serial.println("[FS] open(write) failed");
      return;
    }
    test.println("OK");
    test.close();

    File read = LittleFS.open("/test.txt","r");
    if (!read) {
      Serial.println("[FS] open(read) failed");
      return;
    }
    Serial.print("[FS] read result -> ");
    Serial.println(read.readString());
    read.close();

    Serial.println("[FS] TEST success");
    return;
  }

  // ---- RESET: reload wifi.cfg & reconnect Wi-Fi ----
  if (line == "RESET") {
    Serial.println("[CFG] RESET command received");
    Serial.println("Booting Pico W Switch Pad (soft reset handler)...");
    Serial.println("[CFG] Reloading wifi.cfg...");

    if (!loadWifiConfig()) {
      Serial.println("[CFG] Reload failed. Wi-Fi will be stopped.");
      stopWifiServer();
      Serial.println("[CFG] RESET handler done (Wi-Fi stopped).");
      return;
    }

    if (gWifiConfig.valid) {
      Serial.println("[WiFi] Current config (reloaded):");
      Serial.print("[WiFi] SSID: "); Serial.println(gWifiConfig.ssid);
      Serial.print("[WiFi] IP  : "); Serial.println(gWifiConfig.localIP);
      Serial.print("[WiFi] PORT: "); Serial.println(gWifiConfig.port);
      Serial.print("[WiFi] GW  : "); Serial.println(gWifiConfig.gateway);
      Serial.print("[WiFi] SN  : "); Serial.println(gWifiConfig.subnet);
    } else {
      Serial.println("[WiFi] State: no config -> offline mode");
    }

    Serial.println("[CFG] Applying new Wi-Fi config (reconnect)...");
    stopWifiServer();
    if (startWifiFromConfig()) {
      Serial.println("[CFG] RESET handler done (Wi-Fi reconnected).");
    } else {
      Serial.println("[CFG] RESET handler done (Wi-Fi connect failed).");
    }
    return;
  }

  // ---- return current config ----
  if (line == "CFG GET") {
    Serial.println("[CFG] CURRENT");
    if (gWifiConfig.valid) {
      Serial.print("ssid="); Serial.println(gWifiConfig.ssid);
      Serial.print("pass="); Serial.println(gWifiConfig.password);
      Serial.print("ip=");   Serial.println(gWifiConfig.localIP);
      Serial.print("port="); Serial.println(gWifiConfig.port);
      Serial.print("gw=");   Serial.println(gWifiConfig.gateway);
      Serial.print("sn=");   Serial.println(gWifiConfig.subnet);
    } else {
      Serial.println("none");
    }
    Serial.println("[CFG] CURRENT END");
    return;
  }

  // ---- CFG begin / end ----
  if (line == "CFG BEGIN") {
    cfgReceiving = true;
    resetIncomingCfg();
    Serial.println("[CFG] BEGIN");
    return;
  }

  if (line == "CFG END") {
    cfgReceiving = false;

    if (incomingCfg.ssid.length() == 0) {
      Serial.println("[CFG] INVALID: ssid missing");
      return;
    }
    if (incomingCfg.port.length() == 0) {
      Serial.println("[CFG] INVALID: port missing");
      return;
    }
    if (incomingCfg.localIP == IPAddress() ||
        incomingCfg.gateway == IPAddress() ||
        incomingCfg.subnet == IPAddress()) {
      Serial.println("[CFG] INVALID: ip/gw/sn missing or invalid");
      return;
    }

    if (saveWifiConfig(incomingCfg)) {
      Serial.println("[CFG] SAVED. Send RESET or reboot to apply this Wi-Fi config.");
      gWifiConfig = incomingCfg;
      gWifiConfig.valid = true;
    } else {
      Serial.println("[CFG] SAVE FAILED");
    }
    return;
  }

  if (!cfgReceiving) {
    return;
  }

  // ---- parse key=value lines while in CFG mode ----
  if (line.startsWith("ssid=")) {
    incomingCfg.ssid = line.substring(5);
    incomingCfg.ssid.trim();
    Serial.print("[CFG] ssid="); Serial.println(incomingCfg.ssid);
  } else if (line.startsWith("pass=")) {
    incomingCfg.password = line.substring(5);
    incomingCfg.password.trim();
    Serial.println("[CFG] pass=******");
  } else if (line.startsWith("ip=")) {
    String v = line.substring(3); v.trim();
    IPAddress ip;
    if (ip.fromString(v)) {
      incomingCfg.localIP = ip;
      Serial.print("[CFG] ip="); Serial.println(incomingCfg.localIP);
    } else {
      Serial.print("[CFG] ip parse error: "); Serial.println(v);
    }
  } else if (line.startsWith("gw=")) {
    String v = line.substring(3); v.trim();
    IPAddress gw;
    if (gw.fromString(v)) {
      incomingCfg.gateway = gw;
      Serial.print("[CFG] gw="); Serial.println(incomingCfg.gateway);
    } else {
      Serial.print("[CFG] gw parse error: "); Serial.println(v);
    }
  } else if (line.startsWith("sn=")) {
    String v = line.substring(3); v.trim();
    IPAddress sn;
    if (sn.fromString(v)) {
      incomingCfg.subnet = sn;
      Serial.print("[CFG] sn="); Serial.println(incomingCfg.subnet);
    } else {
      Serial.print("[CFG] sn parse error: "); Serial.println(v);
    }
  } else if (line.startsWith("port=")) {
    incomingCfg.port = line.substring(5);
    incomingCfg.port.trim();
    Serial.print("[CFG] port="); Serial.println(incomingCfg.port);
  } else {
    Serial.print("[CFG] unknown line in CFG mode: ");
    Serial.println(line);
  }
}

// ==============================
// Switch HID / Macro
// ==============================
Adafruit_USBD_HID G_usb_hid;
NSGamepad Gamepad(&G_usb_hid);

// 1800ステップ & 1行最大24文字（終端含め25バイト）
const int    MACRO_MAX_STEPS    = 1800;
const size_t MACRO_LINE_MAXLEN  = 24;

char     macroLines[MACRO_MAX_STEPS][MACRO_LINE_MAXLEN + 1];
int      macroLength      = 0;
bool     macroLoaded      = false;
bool     macroLoading     = false;
bool     macroRunning     = false;

uint32_t macroIntervalMs  = 100;
int      macroIndex       = 0;
uint32_t macroNextTick    = 0;

bool     internalMacroPlayback = false;

// ---- Stick helpers ----
void setLeftStick(uint8_t x, uint8_t y) {
  Gamepad.leftXAxis(x);
  Gamepad.leftYAxis(y);
}

void setRightStick(uint8_t x, uint8_t y) {
  Gamepad.rightXAxis(x);
  Gamepad.rightYAxis(y);
}

void centerSticks() {
  setLeftStick(128, 128);
  setRightStick(128, 128);
}

void dpadCenter() {
  Gamepad.dPad(NSGAMEPAD_DPAD_CENTERED);
}

void sendReport() {
  if (Gamepad.ready()) Gamepad.loop();
}

void clearMacro() {
  macroRunning  = false;
  macroLoading  = false;
  macroLoaded   = false;
  macroLength   = 0;
  macroIndex    = 0;
  if (MACRO_MAX_STEPS > 0) {
    macroLines[0][0] = '\0';
  }
}

// ---- Macro tick ----
void tickMacro() {
  if (!macroRunning || !macroLoaded) return;
  if (!USBDevice.mounted()) return;

  uint32_t now = millis();
  if ((int32_t)(now - macroNextTick) < 0) return;

  // 固定長バッファから取り出す
  char* raw = macroLines[macroIndex];

  // ここだけ一時的に String 化して、既存ロジックをほぼそのまま使う
  String cmd = String(raw);
  String u   = cmd;
  u.trim();
  u.toUpperCase();

  if (u.startsWith("SLEEP ")) {
    int sp = u.indexOf(' ');
    long extra = u.substring(sp + 1).toInt();
    if (extra < 0) extra = 0;
    if (extra > 600000L) extra = 600000L;

    Serial.print("[MACRO SLEEP] ");
    Serial.print(extra);
    Serial.println(" ms");

    macroIndex++;
    if (macroIndex >= macroLength) {
      macroIndex = 0;
    }

    macroNextTick = now + (uint32_t)extra;
    return;
  }

  Serial.print("[MACRO PLAY] ");
  Serial.println(cmd);

  internalMacroPlayback = true;
  handleCommand(cmd, nullptr);
  internalMacroPlayback = false;

  macroIndex++;
  if (macroIndex >= macroLength) {
    macroIndex = 0;
  }
  macroNextTick = now + macroIntervalMs;
}

// ---- Command handler ----
void handleCommand(const String& rawLine, WiFiClient *client) {
  String line = rawLine;
  line.trim();
  if (!line.length()) return;

  String upper = line;
  upper.toUpperCase();

  // MACRO STOP
  if (upper == "MACRO STOP") {
    clearMacro();

    Gamepad.releaseAll();
    dpadCenter();
    centerSticks();
    sendReport();

    Serial.println("[MACRO] STOP & CLEAR");
    return;
  }

  // ignore external commands while macro running
  if (macroRunning && !internalMacroPlayback) {
    Serial.print("[MACRO] running, ignored: ");
    Serial.println(upper);
    return;
  }

  // Macro control
  if (upper.startsWith("MACRO LOAD ")) {
    int s = upper.lastIndexOf(' ');
    int iv = upper.substring(s + 1).toInt();
    if (iv < 10) iv = 10;
    macroIntervalMs = (uint32_t)iv;

    clearMacro();
    macroLoading = true;

    Serial.print("[MACRO] LOAD start, interval = ");
    Serial.print(macroIntervalMs);
    Serial.println(" ms");
    return;
  }

  if (upper == "MACRO END") {
    macroLoading = false;
    macroLoaded  = (macroLength > 0);
    Serial.print("[MACRO] LOAD end. steps = ");
    Serial.println(macroLength);
    return;
  }

  if (upper == "MACRO START") {
    if (macroLoaded && macroLength > 0) {
      macroRunning  = true;
      macroIndex    = 0;
      macroNextTick = millis();
      Serial.println("[MACRO] START");
    } else {
      Serial.println("[MACRO] START requested but no macro loaded");
    }
    return;
  }

  if (macroLoading) {
    if (macroLength < MACRO_MAX_STEPS) {
      // ここで line（すでに trim 済み）を固定長バッファへコピー
      line.toCharArray(macroLines[macroLength], MACRO_LINE_MAXLEN + 1);

      macroLength++;

      Serial.print("[MACRO] step ");
      Serial.print(macroLength);
      Serial.print(": ");
      Serial.println(line);

      // （必要ならオーバー長を検出して警告出すこともできる）
      if (line.length() > MACRO_LINE_MAXLEN) {
        Serial.println("[MACRO] WARNING: line truncated");
      }
    } else {
      Serial.println("[MACRO] buffer full, ignoring extra steps");
    }
    return;
  }

  // ===== Normal commands from here =====

  // BUTTON DOWN
  if      (upper == "BTN A DOWN")       Gamepad.press(NSButton_A);
  else if (upper == "BTN B DOWN")       Gamepad.press(NSButton_B);
  else if (upper == "BTN X DOWN")       Gamepad.press(NSButton_X);
  else if (upper == "BTN Y DOWN")       Gamepad.press(NSButton_Y);

  else if (upper == "BTN L DOWN")       Gamepad.press(NSButton_LeftTrigger);
  else if (upper == "BTN R DOWN")       Gamepad.press(NSButton_RightTrigger);
  else if (upper == "BTN ZL DOWN")      Gamepad.press(NSButton_LeftThrottle);
  else if (upper == "BTN ZR DOWN")      Gamepad.press(NSButton_RightThrottle);

  else if (upper == "BTN PLUS DOWN")    Gamepad.press(NSButton_Plus);
  else if (upper == "BTN MINUS DOWN")   Gamepad.press(NSButton_Minus);

  else if (upper == "BTN HOME DOWN")    Gamepad.press(NSButton_Home);
  else if (upper == "BTN CAPTURE DOWN") Gamepad.press(NSButton_Capture);
  else if (upper == "BTN LSTICK DOWN")  Gamepad.press(NSButton_LeftStick);
  else if (upper == "BTN RSTICK DOWN")  Gamepad.press(NSButton_RightStick);

  // BUTTON UP
  else if (upper == "BTN A UP")         Gamepad.release(NSButton_A);
  else if (upper == "BTN B UP")         Gamepad.release(NSButton_B);
  else if (upper == "BTN X UP")         Gamepad.release(NSButton_X);
  else if (upper == "BTN Y UP")         Gamepad.release(NSButton_Y);

  else if (upper == "BTN L UP")         Gamepad.release(NSButton_LeftTrigger);
  else if (upper == "BTN R UP")         Gamepad.release(NSButton_RightTrigger);
  else if (upper == "BTN ZL UP")        Gamepad.release(NSButton_LeftThrottle);
  else if (upper == "BTN ZR UP")        Gamepad.release(NSButton_RightThrottle);

  else if (upper == "BTN PLUS UP")      Gamepad.release(NSButton_Plus);
  else if (upper == "BTN MINUS UP")     Gamepad.release(NSButton_Minus);

  else if (upper == "BTN HOME UP")      Gamepad.release(NSButton_Home);
  else if (upper == "BTN CAPTURE UP")   Gamepad.release(NSButton_Capture);
  else if (upper == "BTN LSTICK UP")    Gamepad.release(NSButton_LeftStick);
  else if (upper == "BTN RSTICK UP")    Gamepad.release(NSButton_RightStick);

  // ALL UP
  else if (upper == "BTN ALL UP") {
    Gamepad.releaseAll();
    dpadCenter();
    centerSticks();
  }

  // DPad
  else if (upper == "DPAD CENTER")      dpadCenter();
  else if (upper == "DPAD UP")          Gamepad.dPad(NSGAMEPAD_DPAD_UP);
  else if (upper == "DPAD DOWN")        Gamepad.dPad(NSGAMEPAD_DPAD_DOWN);
  else if (upper == "DPAD LEFT")        Gamepad.dPad(NSGAMEPAD_DPAD_LEFT);
  else if (upper == "DPAD RIGHT")       Gamepad.dPad(NSGAMEPAD_DPAD_RIGHT);
  else if (upper == "DPAD UPLEFT")      Gamepad.dPad(NSGAMEPAD_DPAD_UP_LEFT);
  else if (upper == "DPAD UPRIGHT")     Gamepad.dPad(NSGAMEPAD_DPAD_UP_RIGHT);
  else if (upper == "DPAD DOWNLEFT")    Gamepad.dPad(NSGAMEPAD_DPAD_DOWN_LEFT);
  else if (upper == "DPAD DOWNRIGHT")   Gamepad.dPad(NSGAMEPAD_DPAD_DOWN_RIGHT);

  // Stick presets
  else if (upper == "LSTICK CENTER") setLeftStick(128,128);
  else if (upper == "LSTICK UP")     setLeftStick(128,0);
  else if (upper == "LSTICK DOWN")   setLeftStick(128,255);
  else if (upper == "LSTICK LEFT")   setLeftStick(0,128);
  else if (upper == "LSTICK RIGHT")  setLeftStick(255,128);

  else if (upper == "RSTICK CENTER") setRightStick(128,128);
  else if (upper == "RSTICK UP")     setRightStick(128,0);
  else if (upper == "RSTICK DOWN")   setRightStick(128,255);
  else if (upper == "RSTICK LEFT")   setRightStick(0,128);
  else if (upper == "RSTICK RIGHT")  setRightStick(255,128);

  // Left stick numeric
  else if (upper.startsWith("LSTICK ")) {
    int s1 = line.indexOf(' ');
    int s2 = line.indexOf(' ', s1 + 1);
    if (s1 > 0 && s2 > s1) {
      uint8_t x = line.substring(s1 + 1, s2).toInt();
      uint8_t y = line.substring(s2 + 1).toInt();
      setLeftStick(x, y);
    }
  }

  // Right stick numeric
  else if (upper.startsWith("RSTICK ")) {
    int s1 = line.indexOf(' ');
    int s2 = line.indexOf(' ', s1 + 1);
    if (s1 > 0 && s2 > s1) {
      uint8_t x = line.substring(s1 + 1, s2).toInt();
      uint8_t y = line.substring(s2 + 1).toInt();
      setRightStick(x, y);
    }
  }

  // VERSION (Wi-Fi & Serial)
  else if (upper == "VERSION") {
    Serial.print("[FW] VERSION ");
    Serial.println(FW_VERSION);
    if (client && client->connected()) {
      client->println(FW_VERSION);
    }
  }

  sendReport();
}

// ==============================
// Wi-Fi task (non-blocking)
// ==============================
void processWifiClientTask() {
  if (!wifiStarted || !gServer) return;

  // 前回のクライアント切断チェック
  if (gCurrentClient && !gCurrentClient.connected()) {
    Serial.println("[WiFi] Client disconnected");
    gCurrentClient.stop();
    gWifiLineBuffer = "";
  }

  // クライアントがいなければ accept
  if (!gCurrentClient || !gCurrentClient.connected()) {
    WiFiClient newClient = gServer->accept();
    if (newClient) {
      gCurrentClient = newClient;
      gWifiLineBuffer = "";
      Serial.println("[WiFi] Client connected");
    }
  }

  // 接続済みなら I/O 処理
  if (gCurrentClient && gCurrentClient.connected()) {
    while (gCurrentClient.available()) {
      char c = gCurrentClient.read();
      if (c == '\n') {
        String line = gWifiLineBuffer;
        line.trim();
        if (line.length()) {
          Serial.print("CMD: ");
          Serial.println(line);
          handleCommand(line, &gCurrentClient);
        }
        gWifiLineBuffer = "";
      } else if (c != '\r') {
        gWifiLineBuffer += c;
      }
      yield();
    }
  }
}

// ==============================
// HID & Macro Task
// ==============================
void processHidAndMacroTask() {
  if (Gamepad.ready()) {
    Gamepad.loop();
  }

  tickMacro();

  if (macroRunning && !USBDevice.mounted()) {
    clearMacro();
    Gamepad.releaseAll();
    dpadCenter();
    centerSticks();
    sendReport();
    Serial.println("[MACRO] stopped due to USB disconnect");
  }
}

// ==============================
// setup / loop
// ==============================
void setup() {
  Serial.begin(115200);
  delay(500);        // BOOTSEL直後のUSBシリアル安定化用
  Gamepad.begin();
  delay(500);        // HID初期化待機
  Serial.println("Booting Pico W Switch Pad...");

  // よく伸びるバッファの事前確保（サイズは適当に調整）
  serialLine.reserve(256);
  gWifiLineBuffer.reserve(256);

  if (loadWifiConfig()) {
    if (!startWifiFromConfig()) {
      Serial.println("[WiFi] connect failed at boot -> offline mode");
    }
  } else {
    Serial.println("[WiFi] no config -> Wi-Fi will not be used");
  }
}

void loop() {
  processSerialInput();
  processWifiClientTask();
  processHidAndMacroTask();

  yield();
  delay(0);
}

