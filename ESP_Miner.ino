/*
   Duino-Coin ESP32 Miner - ULTIMATE EDITION
   Kết hợp:
   - Poolpicker tự động (từ official code)
   - Non-blocking dual-core mining
   - NAT Internet sharing
   - Web dashboard đẹp + Captive portal
   - Hỗ trợ màn hình OLED/LCD (tùy chọn)
*/

#pragma GCC optimize("-Ofast")

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Ticker.h>
#include <ArduinoOTA.h>
#include <lwip/netif.h>
#include <lwip/ip4_addr.h>
#include <dhcpserver.h>
#include <lwip/napt.h>

#include "MiningJob.h"
#include "Settings.h"

// ================= CONFIG =================
#define DNS_PORT 53
const char* DEFAULT_AP_SSID = "ESP32_Miner";
const char* DEFAULT_AP_PASS = "12345678";

IPAddress AP_IP(192, 168, 4, 1);
IPAddress AP_GATEWAY(192, 168, 4, 1);
IPAddress AP_SUBNET(255, 255, 255, 0);

// ================= GLOBAL =================
DNSServer dns;
WebServer server(80);
Preferences prefs;

String sta_ssid, sta_pass;
String ap_ssid, ap_pass;

bool internetOK = false;
bool natEnabled = false;
unsigned long uptimeStart = 0;

// State machine WiFi
enum WiFiState {
  WIFI_IDLE,
  WIFI_CONNECTING,
  WIFI_CONNECTED,
  WIFI_FAILED
};
WiFiState wifiState = WIFI_IDLE;
unsigned long lastAttempt = 0;
unsigned long lastInternetCheck = 0;

// Mining jobs
MiningJob *job[2];
TaskHandle_t minerTask0;
TaskHandle_t minerTask1;
MiningConfig *configuration;

// ================= MINING GLOBALS =================
// Các biến này được định nghĩa trong Settings.h
extern unsigned int hashrate;
extern unsigned int hashrate_core_two;
extern unsigned long share_count;
extern unsigned long accepted_share_count;
extern unsigned long difficulty;
extern String node_id;
extern unsigned int ping;

// ================= NAT FUNCTIONS =================
void setupNAT() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0,0,0,0)) {
    ip_napt_init(4096, 4096);
    bool ret = ip_napt_enable(WiFi.localIP(), 1);
    natEnabled = ret;
    if (ret) {
      Serial.println("✅ NAT Enabled - Internet sharing activated!");
      Serial.printf("   STA IP: %s -> AP IP: %s\n", 
                    WiFi.localIP().toString().c_str(), 
                    AP_IP.toString().c_str());
    } else {
      Serial.println("❌ NAT Failed to enable!");
    }
  }
}

void disableNAT() {
  if (natEnabled) {
    ip_napt_disable();
    natEnabled = false;
    Serial.println("❌ NAT Disabled");
  }
}

// ================= POOLPICKER =================
void UpdateHostPort(String input) {
  DynamicJsonDocument doc(256);
  deserializeJson(doc, input);
  const char *name = doc["name"];
  
  configuration->host = doc["ip"].as<String>();
  configuration->port = doc["port"].as<int>();
  node_id = String(name);
  
  // Cập nhật cho cả 2 job
  job[0]->config->host = configuration->host;
  job[0]->config->port = configuration->port;
  job[1]->config->host = configuration->host;
  job[1]->config->port = configuration->port;
  
  Serial.println("✅ Poolpicker selected node: " + node_id);
  Serial.printf("   Host: %s:%d\n", configuration->host.c_str(), configuration->port);
}

String httpGetString(String URL) {
  String payload = "";
  WiFiClientSecure client;
  HTTPClient https;
  client.setInsecure();
  
  https.begin(client, URL);
  https.addHeader("Accept", "*/*");
  
  int httpCode = https.GET();
  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
    payload = https.getString();
  } else {
    Serial.printf("Error fetching node: %s\n", https.errorToString(httpCode).c_str());
  }
  https.end();
  return payload;
}

void SelectNode() {
  String input = "";
  int waitTime = 1;
  
  while (input == "") {
    Serial.println("Fetching mining node from poolpicker in " + String(waitTime) + "s");
    delay(waitTime * 1000);
    
    input = httpGetString("https://server.duinocoin.com/getPool");
    
    waitTime *= 2;
    if (waitTime > 32) {
      Serial.println("Using fallback node");
      configuration->host = "server.duinocoin.com";
      configuration->port = 2811;
      node_id = "fallback";
      break;
    }
  }
  
  if (input != "") {
    UpdateHostPort(input);
  }
}

// ================= MINING TASK =================
void miningTask(void *param) {
  MiningJob *j = (MiningJob*)param;
  while (true) {
    if (j) j->mine();
    taskYIELD();
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  uptimeStart = millis();
  
  Serial.println("\n\n╔════════════════════════════════════════════════════╗");
  Serial.println("║   Duino-Coin ESP32 Miner - ULTIMATE EDITION      ║");
  Serial.println("║   Poolpicker | Dual-Core | NAT | Web Dashboard   ║");
  Serial.println("╚════════════════════════════════════════════════════╝\n");
  
  // Đọc cấu hình từ Preferences
  prefs.begin("miner", false);
  sta_ssid = prefs.getString("sta_ssid", "");
  sta_pass = prefs.getString("sta_pass", "");
  ap_ssid  = prefs.getString("ap_ssid", DEFAULT_AP_SSID);
  ap_pass  = prefs.getString("ap_pass", DEFAULT_AP_PASS);
  
  // Khởi tạo configuration
  configuration = new MiningConfig(DUCO_USER, RIG_IDENTIFIER, MINER_KEY);
  
  // Khởi tạo Access Point
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
  WiFi.softAP(ap_ssid.c_str(), ap_pass.c_str());
  Serial.println("✅ AP Started: " + ap_ssid);
  
  // DNS Server (Captive Portal)
  dns.start(DNS_PORT, "*", AP_IP);
  Serial.println("✅ DNS Server Started");
  
  // Khởi tạo mining jobs
  job[0] = new MiningJob(0, new MiningConfig(DUCO_USER, RIG_IDENTIFIER, MINER_KEY));
  job[1] = new MiningJob(1, new MiningConfig(DUCO_USER, RIG_IDENTIFIER, MINER_KEY));
  
  // Web Server
  setupWeb();
  
  // mDNS
  if (MDNS.begin("esp32miner")) {
    Serial.println("✅ mDNS: http://esp32miner.local");
  }
  
  // OTA
  ArduinoOTA.setHostname(RIG_IDENTIFIER);
  ArduinoOTA.begin();
  
  // Kết nối WiFi STA
  if (sta_ssid.length() > 0) {
    startConnect();
  } else {
    Serial.println("\n⚠️ Chưa có cấu hình WiFi!");
    Serial.printf("📱 Kết nối vào AP: %s\n", ap_ssid.c_str());
    Serial.printf("🌐 Mở trình duyệt: http://%s\n", AP_IP.toString().c_str());
    Serial.println("🔧 Cấu hình WiFi nhà để bắt đầu đào và chia sẻ internet\n");
  }
  
  // LED báo hiệu
  pinMode(LED_BUILTIN, OUTPUT);
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_BUILTIN, LOW);
    delay(100);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(100);
  }
  
  // Dual-core mining tasks (non-blocking)
  xTaskCreatePinnedToCore(miningTask, "miner0", 8192, job[0], 1, &minerTask0, 0);
  xTaskCreatePinnedToCore(miningTask, "miner1", 8192, job[1], 1, &minerTask1, 1);
  
  Serial.println("✅ ESP32 Miner Ready!");
  Serial.println("📡 Dual-core mining | NAT ready | Web: http://192.168.4.1");
}

// ================= LOOP =================
void loop() {
  dns.processNextRequest();
  server.handleClient();
  ArduinoOTA.handle();
  
  handleWiFiState();
  checkInternetLoop();
  
  delay(2); // Non-blocking
}

// ================= WIFI MANAGEMENT =================
void startConnect() {
  if (sta_ssid.length() == 0) return;
  disableNAT();
  WiFi.begin(sta_ssid.c_str(), sta_pass.c_str());
  wifiState = WIFI_CONNECTING;
  lastAttempt = millis();
  Serial.printf("📡 Connecting to: %s\n", sta_ssid.c_str());
}

void handleWiFiState() {
  switch (wifiState) {
    case WIFI_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        wifiState = WIFI_CONNECTED;
        Serial.println("✅ WiFi Connected!");
        Serial.printf("   IP: %s\n", WiFi.localIP().toString());
        Serial.printf("   Signal: %d dBm\n", WiFi.RSSI());
        
        setupNAT();
        
        // Chọn node tốt nhất từ poolpicker
        SelectNode();
        
        // Kết nối đến node
        job[0]->connectToNode();
        job[1]->connectToNode();
      } else if (millis() - lastAttempt > 15000) {
        wifiState = WIFI_FAILED;
        Serial.println("❌ WiFi Connection Failed!");
      }
      break;
      
    case WIFI_FAILED:
      if (millis() - lastAttempt > 30000) {
        Serial.println("🔄 Retrying WiFi connection...");
        startConnect();
      }
      break;
      
    case WIFI_CONNECTED:
      if (WiFi.status() != WL_CONNECTED) {
        wifiState = WIFI_FAILED;
        lastAttempt = millis();
        Serial.println("⚠️ WiFi Disconnected!");
        disableNAT();
      }
      break;
      
    default: break;
  }
}

bool checkInternet() {
  HTTPClient http;
  http.setTimeout(3000);
  http.begin("http://clients3.google.com/generate_204");
  int code = http.GET();
  http.end();
  return (code == 204);
}

void checkInternetLoop() {
  if (wifiState != WIFI_CONNECTED) return;
  if (millis() - lastInternetCheck > 30000) {
    lastInternetCheck = millis();
    bool now = checkInternet();
    if (now != internetOK) {
      internetOK = now;
      if (internetOK) {
        Serial.println("🌍 Internet OK - AP clients can now browse");
        if (!natEnabled && WiFi.status() == WL_CONNECTED) setupNAT();
      } else {
        Serial.println("⚠️ Internet LOST");
      }
    }
  }
}

// ================= WEB SERVER =================
void setupWeb() {
  server.on("/", handleRoot);
  server.on("/scan", handleScan);
  server.on("/save-sta", HTTP_POST, handleSaveSTA);
  server.on("/save-ap", HTTP_POST, handleSaveAP);
  server.on("/reset", handleReset);
  server.on("/reboot", handleReboot);
  server.on("/status", handleStatus);
  server.on("/mining", handleMiningStatus);
  server.on("/nat-status", handleNATStatus);
  
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1", true);
    server.send(302, "text/plain", "");
  });
  
  server.begin();
  Serial.println("✅ Web Server Started");
}

void handleRoot() {
  String html;
  html.reserve(8000);
  unsigned long uptime = (millis() - uptimeStart) / 1000;
  float hashrate_khs = (hashrate + hashrate_core_two) / 1000.0;
  
  html += "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='10'>";
  html += "<title>ESP32 Miner - Ultimate</title>";
  html += "<style>";
  html += "*{margin:0;padding:0;box-sizing:border-box;}";
  html += "body{font-family:'Segoe UI',Arial;background:linear-gradient(135deg,#1a1a2e 0%,#16213e 100%);min-height:100vh;padding:20px;}";
  html += ".container{max-width:900px;margin:0 auto;}";
  html += ".card{background:rgba(255,255,255,0.1);backdrop-filter:blur(10px);border-radius:15px;padding:20px;margin-bottom:20px;border:1px solid rgba(255,255,255,0.2);}";
  html += "h1{text-align:center;color:#00d4ff;margin-bottom:20px;}";
  html += "h2{color:#eee;margin-bottom:15px;border-bottom:1px solid #00d4ff;padding-bottom:10px;}";
  html += ".status{display:inline-block;padding:5px 15px;border-radius:20px;font-weight:bold;}";
  html += ".online{background:#00ff8822;color:#0f0;border:1px solid #0f0;}";
  html += ".offline{background:#ff444422;color:#f44;border:1px solid #f44;}";
  html += ".nat-enabled{background:#00aaff22;color:#0af;border:1px solid #0af;}";
  html += "table{width:100%;border-collapse:collapse;}";
  html += "td,th{padding:12px;text-align:left;border-bottom:1px solid rgba(255,255,255,0.1);}";
  html += "th{color:#00d4ff;}";
  html += "input{width:100%;padding:10px;margin:10px 0;border-radius:8px;border:none;background:rgba(255,255,255,0.1);color:white;}";
  html += "input::placeholder{color:#888;}";
  html += "button{padding:10px 20px;margin:5px;background:#00d4ff;color:#1a1a2e;border:none;border-radius:8px;cursor:pointer;font-weight:bold;}";
  html += "button:hover{background:#00aacc;}";
  html += ".grid-2{display:grid;grid-template-columns:1fr 1fr;gap:20px;}";
  html += ".value{color:#00d4ff;font-weight:bold;font-size:1.2em;}";
  html += ".footer{text-align:center;color:#888;margin-top:20px;font-size:12px;}";
  html += ".info-box{background:#00d4ff22;border-left:4px solid #00d4ff;padding:10px;margin:10px 0;border-radius:5px;}";
  html += "@media (max-width:600px){.grid-2{grid-template-columns:1fr;}}";
  html += "</style></head><body>";
  
  html += "<div class='container'>";
  html += "<h1>⛏️ ESP32 Duino-Coin Miner - ULTIMATE</h1>";
  html += "<p style='text-align:center;color:#888;margin-bottom:20px;'>⚡ Poolpicker | Dual-Core | NAT Router | v" + String(SOFTWARE_VERSION) + "</p>";
  
  // Internet Sharing Status
  html += "<div class='card'>";
  html += "<h2>🌐 Internet Sharing Status</h2>";
  html += "<div class='info-box'><strong>📡 Kết nối vào WiFi ESP32 để có internet</strong><br>📌 Sau khi cấu hình WiFi nhà, AP sẽ tự động chia sẻ internet</div>";
  html += "<table>";
  html += "<tr><th>WiFi STA</th><td>" + String((wifiState == WIFI_CONNECTED) ? "<span class='status online'>Connected</span>" : "<span class='status offline'>Disconnected</span>") + "</td></tr>";
  html += "<tr><th>Internet</th><td>" + String(internetOK ? "<span class='status online'>Available</span>" : "<span class='status offline'>No</span>") + "</td></tr>";
  html += "<tr><th>NAT Sharing</th><td>" + String(natEnabled ? "<span class='status nat-enabled'>✓ Active</span>" : "<span class='status offline'>✗ Inactive</span>") + "</td></tr>";
  html += "<tr><th>Clients</th><td>" + String(WiFi.softAPgetStationNum()) + " device(s)</td></tr>";
  html += "</table></div>";
  
  // Mining Status
  html += "<div class='card'>";
  html += "<h2>⛏️ Mining Status</h2>";
  html += "<table>";
  html += "<tr><th>Node</th><td><strong>" + node_id + "</strong></td></tr>";
  html += "<tr><th>Hashrate Core 0</th><td>" + String(hashrate / 1000.0, 2) + " kH/s</td></tr>";
  html += "<tr><th>Hashrate Core 1</th><td>" + String(hashrate_core_two / 1000.0, 2) + " kH/s</td></tr>";
  html += "<tr><th>Total Hashrate</th><td><span class='value'>" + String(hashrate_khs, 2) + " kH/s</span></td></tr>";
  html += "<tr><th>Shares</th><td>" + String(share_count) + " (Accepted: " + String(accepted_share_count) + ")</td></tr>";
  html += "<tr><th>Difficulty</th><td>" + String(difficulty / 100) + "</td></tr>";
  html += "<tr><th>Ping</th><td>" + String(ping) + " ms</td></tr>";
  html += "</table></div>";
  
  html += "<div class='grid-2'>";
  
  // WiFi Configuration
  html += "<div class='card'>";
  html += "<h2>📶 WiFi Configuration</h2>";
  html += "<p><small>Cấu hình WiFi nhà để có internet và CHIA SẺ qua AP</small></p>";
  html += "<form method='POST' action='/save-sta'>";
  html += "<input type='text' name='ssid' placeholder='WiFi Name (SSID)' required>";
  html += "<input type='password' name='pass' placeholder='Password'>";
  html += "<button type='submit'>💾 Save & Connect</button>";
  html += "</form>";
  html += "<p><a href='/scan'><button type='button'>🔍 Scan Networks</button></a></p>";
  if (wifiState == WIFI_CONNECTED) html += "<p style='margin-top:10px;color:#0f0;'>✓ Connected to: " + sta_ssid + "</p>";
  html += "</div>";
  
  // AP Configuration
  html += "<div class='card'>";
  html += "<h2>📱 Access Point</h2>";
  html += "<form method='POST' action='/save-ap'>";
  html += "<input type='text' name='ssid' placeholder='AP SSID' value='" + ap_ssid + "' required>";
  html += "<input type='text' name='pass' placeholder='AP Password' value='" + ap_pass + "'>";
  html += "<button type='submit'>💾 Save AP Config</button>";
  html += "</form>";
  html += "<p style='margin-top:10px;'>🌐 Kết nối vào <strong>" + ap_ssid + "</strong> để có internet!</p>";
  html += "</div>";
  
  html += "</div>";
  
  // System Info
  html += "<div class='card'>";
  html += "<h2>📊 System Info</h2>";
  html += "<table>";
  html += "<tr><th>AP IP</th><td>" + AP_IP.toString() + "</td></tr>";
  html += "<tr><th>STA IP</th><td>" + String(wifiState == WIFI_CONNECTED ? WiFi.localIP().toString() : "Not connected") + "</td></tr>";
  html += "<tr><th>Uptime</th><td>" + String(uptime / 3600) + "h " + String((uptime % 3600) / 60) + "m " + String(uptime % 60) + "s</td></tr>";
  html += "<tr><th>Free Heap</th><td>" + String(ESP.getFreeHeap() / 1024) + " KB</td></tr>";
  html += "<tr><th>CPU Freq</th><td>" + String(getCpuFrequencyMhz()) + " MHz</td></tr>";
  html += "</table></div>";
  
  // Actions
  html += "<div class='card'>";
  html += "<h2>⚙️ Actions</h2>";
  html += "<a href='/mining'><button>📊 Mining JSON</button></a> ";
  html += "<a href='/nat-status'><button>🌐 NAT Status</button></a> ";
  html += "<a href='/reset'><button onclick='return confirm(\"Reset all settings?\")'>🗑️ Factory Reset</button></a> ";
  html += "<a href='/reboot'><button onclick='return confirm(\"Reboot ESP32?\")'>🔄 Reboot</button></a>";
  html += "</div>";
  
  html += "<div class='footer'>";
  html += "<p>ESP32 Duino-Coin Miner Ultimate | " + String(__DATE__) + " " + String(__TIME__) + "</p>";
  html += "<p>🔗 AP: " + ap_ssid + " | http://" + AP_IP.toString() + " | http://esp32miner.local</p>";
  html += "<p>⚡ Poolpicker | Dual-Core Mining | NAT Router</p>";
  html += "</div></div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleSaveSTA() {
  String new_ssid = server.arg("ssid");
  String new_pass = server.arg("pass");
  new_ssid.replace("+", " ");
  
  if (new_ssid.length() > 0) {
    prefs.putString("sta_ssid", new_ssid);
    prefs.putString("sta_pass", new_pass);
    sta_ssid = new_ssid;
    sta_pass = new_pass;
    startConnect();
    
    server.send(200, "text/html", "<html><head><meta http-equiv='refresh' content='3;url=/'><style>body{background:#1a1a2e;color:white;text-align:center;padding:50px;}</style></head><body><h1>✅ Saved!</h1><p>Connecting to: " + new_ssid + "</p><p>🌐 Internet will be shared after connection</p></body></html>");
  } else {
    server.send(400, "text/plain", "SSID required!");
  }
}

void handleSaveAP() {
  String new_ap_ssid = server.arg("ssid");
  String new_ap_pass = server.arg("pass");
  new_ap_ssid.replace("+", " ");
  
  if (new_ap_ssid.length() > 0) {
    prefs.putString("ap_ssid", new_ap_ssid);
    prefs.putString("ap_pass", new_ap_pass);
    ap_ssid = new_ap_ssid;
    ap_pass = new_ap_pass;
    WiFi.softAP(ap_ssid.c_str(), ap_pass.c_str());
    
    server.send(200, "text/html", "<html><head><meta http-equiv='refresh' content='2;url=/'><style>body{background:#1a1a2e;color:white;text-align:center;padding:50px;}</style></head><body><h1>✅ AP Saved!</h1><p>New AP: " + new_ap_ssid + "</p></body></html>");
  } else {
    server.send(400, "text/plain", "AP SSID required!");
  }
}

void handleScan() {
  String html;
  html.reserve(4000);
  html += "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:'Segoe UI',Arial;padding:20px;background:#1a1a2e;color:white;} h2{color:#00d4ff;} li{cursor:pointer;margin:5px 0;padding:12px;background:#16213e;border-radius:8px;list-style:none;} li:hover{background:#00d4ff22;} button{padding:10px 20px;background:#00d4ff;color:#1a1a2e;border:none;border-radius:8px;cursor:pointer;} input{width:100%;padding:10px;margin:10px 0;border-radius:8px;background:#16213e;color:white;border:1px solid #00d4ff;}</style>";
  html += "<script>function pick(ssid){document.getElementById('ssid').value=ssid;alert('✅ Selected: '+ssid);}</script>";
  html += "</head><body><h2>📡 WiFi Networks</h2><p>🔍 Click vào WiFi để chọn:</p><ul>";
  
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    html += "<li onclick=\"pick('" + WiFi.SSID(i) + "')\">📶 " + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + " dBm)";
    if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) html += " 🔓";
    html += "</li>";
  }
  html += "</ul><form method='POST' action='/save-sta'>";
  html += "<input type='text' id='ssid' name='ssid' placeholder='Hoặc nhập tên WiFi'>";
  html += "<input type='password' name='pass' placeholder='Mật khẩu'>";
  html += "<button type='submit'>💾 Lưu & Kết nối</button></form>";
  html += "<p><a href='/'><button>← Quay lại</button></a></p></body></html>";
  server.send(200, "text/html", html);
}

void handleMiningStatus() {
  String json = "{";
  json += "\"node\":\"" + node_id + "\",";
  json += "\"hashrate_core0\":" + String(hashrate / 1000.0, 2) + ",";
  json += "\"hashrate_core1\":" + String(hashrate_core_two / 1000.0, 2) + ",";
  json += "\"hashrate_total\":" + String((hashrate + hashrate_core_two) / 1000.0, 2) + ",";
  json += "\"shares\":" + String(share_count) + ",";
  json += "\"accepted_shares\":" + String(accepted_share_count) + ",";
  json += "\"difficulty\":" + String(difficulty / 100) + ",";
  json += "\"ping\":" + String(ping);
  json += "}";
  server.send(200, "application/json", json);
}

void handleStatus() {
  String json = "{";
  json += "\"wifi\":" + String((wifiState == WIFI_CONNECTED) ? "\"connected\"" : "\"disconnected\"") + ",";
  json += "\"internet\":" + String(internetOK ? "true" : "false") + ",";
  json += "\"nat_enabled\":" + String(natEnabled ? "true" : "false") + ",";
  json += "\"ap_ssid\":\"" + ap_ssid + "\",";
  json += "\"clients\":" + String(WiFi.softAPgetStationNum()) + ",";
  json += "\"uptime\":" + String((millis() - uptimeStart) / 1000);
  json += "}";
  server.send(200, "application/json", json);
}

void handleNATStatus() {
  String json = "{";
  json += "\"nat_enabled\":" + String(natEnabled ? "true" : "false") + ",";
  json += "\"sta_ip\":\"" + (wifiState == WIFI_CONNECTED ? WiFi.localIP().toString() : "0.0.0.0") + "\",";
  json += "\"ap_ip\":\"" + AP_IP.toString() + "\",";
  json += "\"clients\":" + String(WiFi.softAPgetStationNum()) + ",";
  json += "\"internet\":" + String(internetOK ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleReset() {
  prefs.clear();
  disableNAT();
  server.send(200, "text/html", "<html><head><meta http-equiv='refresh' content='3'><style>body{background:#1a1a2e;color:white;text-align:center;padding:50px;}</style></head><body><h1>🗑️ Factory Reset</h1><p>ESP32 will restart...</p></body></html>");
  delay(1000);
  ESP.restart();
}

void handleReboot() {
  disableNAT();
  server.send(200, "text/html", "<html><head><meta http-equiv='refresh' content='3'><style>body{background:#1a1a2e;color:white;text-align:center;padding:50px;}</style></head><body><h1>🔄 Rebooting...</h1><p>Please wait 10 seconds...</p></body></html>");
  delay(1000);
  ESP.restart();
}
