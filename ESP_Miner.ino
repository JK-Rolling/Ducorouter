/*
   Duino-Coin ESP32 Miner with Access Point & Internet Sharing (NAT)
   - Đào Duino-Coin trên cả 2 core (ESP32 dual-core)
   - Tạo Access Point (AP) để cấu hình WiFi và giám sát
   - CHIA SẺ INTERNET từ WiFi nhà qua AP (NAT)
   - Web dashboard hiển thị thông tin đào
   - Lưu cấu hình vào Preferences
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

// ================= CONFIG AP =================
#define DNS_PORT 53
const char* DEFAULT_AP_SSID = "ESP32_Miner";
const char* DEFAULT_AP_PASS = "12345678";

IPAddress AP_IP(192, 168, 4, 1);
IPAddress AP_GATEWAY(192, 168, 4, 1);
IPAddress AP_SUBNET(255, 255, 255, 0);
IPAddress AP_DNS(8, 8, 8, 8);  // Google DNS

// ================= GLOBAL =================
DNSServer dns;
WebServer server(80);
Preferences prefs;

// Cấu hình WiFi STA (kết nối Internet)
String sta_ssid, sta_pass;
// Cấu hình AP (điểm truy cập)
String ap_ssid, ap_pass;

bool internetOK = false;
bool natEnabled = false;
unsigned long uptimeStart = 0;

// State machine cho kết nối STA
enum WiFiState {
  WIFI_IDLE,
  WIFI_CONNECTING,
  WIFI_CONNECTED,
  WIFI_FAILED
};
WiFiState wifiState = WIFI_IDLE;
unsigned long lastAttempt = 0;
unsigned long lastInternetCheck = 0;

// ================= MINING GLOBALS =================
MiningJob *job[2];  // Core 0 và Core 1
float hashrate_total = 0;
unsigned long share_count = 0;
unsigned long accepted_share_count = 0;
String node_id = "";
unsigned long difficulty = 0;
unsigned long ping = 0;

// ================= NAT FUNCTIONS =================
void setupNAT() {
  // Kiểm tra xem STA đã có IP chưa
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0,0,0,0)) {
    ip4_addr_t nat_ip = WiFi.localIP();
    ip4_addr_t ap_ip = AP_IP;
    
    // Enable NAT (Network Address Translation)
    // Đây là hàm quan trọng để chia sẻ internet
    bool ret = ip_napt_enable(nat_ip, 1);
    
    if (ret) {
      natEnabled = true;
      Serial.println("✅ NAT Enabled - Internet sharing activated!");
      Serial.printf("   STA IP: %s -> AP IP: %s\n", 
                    WiFi.localIP().toString().c_str(), 
                    AP_IP.toString().c_str());
    } else {
      natEnabled = false;
      Serial.println("❌ NAT Failed to enable!");
    }
  } else {
    natEnabled = false;
    Serial.println("⚠️ Cannot enable NAT: STA not connected");
  }
}

void disableNAT() {
  if (natEnabled) {
    ip_napt_disable();
    natEnabled = false;
    Serial.println("❌ NAT Disabled");
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  uptimeStart = millis();

  Serial.println("\n\n╔════════════════════════════════════════════════════╗");
  Serial.println("║   Duino-Coin ESP32 Miner + AP + INTERNET SHARE   ║");
  Serial.println("║   Dual-Core Mining | Web Dashboard | NAT Router  ║");
  Serial.println("╚════════════════════════════════════════════════════╝\n");

  // Đọc cấu hình từ Preferences
  prefs.begin("miner", false);
  sta_ssid = prefs.getString("sta_ssid", "");
  sta_pass = prefs.getString("sta_pass", "");
  ap_ssid  = prefs.getString("ap_ssid", DEFAULT_AP_SSID);
  ap_pass  = prefs.getString("ap_pass", DEFAULT_AP_PASS);

  // Khởi tạo Access Point (luôn chạy)
  setupAP();
  setupDNS();
  setupWeb();
  setupMDNS();

  // Khởi tạo OTA
  ArduinoOTA.setHostname(RIG_IDENTIFIER);
  ArduinoOTA.begin();

  // Khởi tạo mining jobs
  job[0] = new MiningJob(0, new MiningConfig(DUCO_USER, RIG_IDENTIFIER, MINER_KEY));
  job[1] = new MiningJob(1, new MiningConfig(DUCO_USER, RIG_IDENTIFIER, MINER_KEY));

  // Kết nối WiFi STA nếu có cấu hình
  if (sta_ssid.length() > 0) {
    startConnect();
  } else {
    Serial.println("\n⚠️ Chưa có cấu hình WiFi!");
    Serial.printf("📱 Kết nối vào AP: %s\n", ap_ssid.c_str());
    Serial.printf("🌐 Mở trình duyệt: http://%s\n", AP_IP.toString().c_str());
    Serial.println("🔧 Cấu hình WiFi nhà để bắt đầu đào và chia sẻ internet\n");
  }

  // LED báo hiệu sẵn sàng
  pinMode(LED_BUILTIN, OUTPUT);
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_BUILTIN, LOW);
    delay(100);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(100);
  }

  Serial.println("✅ ESP32 Miner Ready!");
  Serial.println("📡 Kết nối vào AP để có internet (sau khi cấu hình WiFi nhà)");
}

// ================= LOOP =================
void loop() {
  // Xử lý AP và Web
  dns.processNextRequest();
  server.handleClient();
  ArduinoOTA.handle();

  // Quản lý kết nối WiFi STA
  handleWiFiState();
  checkInternetLoop();

  // Mining trên core 0 (hàm mine() có delay bên trong)
  if (job[0]) job[0]->mine();

  delay(10);
}

// ================= ACCESS POINT =================
void setupAP() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
  WiFi.softAP(ap_ssid.c_str(), ap_pass.c_str());

  Serial.println("✅ AP Started");
  Serial.printf("   SSID: %s\n", ap_ssid.c_str());
  Serial.printf("   Pass: %s\n", ap_pass.c_str());
  Serial.printf("   IP: %s\n", AP_IP.toString().c_str());
  Serial.println("   🌐 Devices connected to this AP will have internet (when STA connected)");
}

void setupDNS() {
  dns.start(DNS_PORT, "*", AP_IP);
  Serial.println("✅ DNS Server Started (Captive Portal)");
}

void setupMDNS() {
  if (MDNS.begin("esp32miner")) {
    Serial.println("✅ mDNS: http://esp32miner.local");
  }
}

// ================= WiFi STA MANAGEMENT =================
void startConnect() {
  if (sta_ssid.length() == 0) return;
  
  // Disable NAT trước khi reconnect
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
        Serial.printf("   Gateway: %s\n", WiFi.gatewayIP().toString());
        Serial.printf("   DNS: %s\n", WiFi.dnsIP().toString());
        Serial.printf("   Signal: %d dBm\n", WiFi.RSSI());
        
        // Kích hoạt NAT để chia sẻ internet qua AP
        setupNAT();
        
        // Kết nối đến node Duino-Coin sau khi có WiFi
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
        Serial.println("⚠️ WiFi Disconnected! Internet sharing stopped.");
        disableNAT();
      }
      break;
      
    default: break;
  }
}

// ================= INTERNET CHECK =================
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
        // Đảm bảo NAT vẫn đang hoạt động
        if (!natEnabled && WiFi.status() == WL_CONNECTED) {
          setupNAT();
        }
      } else {
        Serial.println("⚠️ Internet LOST - AP clients cannot browse");
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
    // Captive portal: redirect all requests to main page
    server.sendHeader("Location", "http://192.168.4.1", true);
    server.send(302, "text/plain", "");
  });

  server.begin();
  Serial.println("✅ Web Server Started");
}

// ================= WEB HANDLERS =================
void handleRoot() {
  String html;
  html.reserve(7000);
  unsigned long uptime = (millis() - uptimeStart) / 1000;
  float hashrate_khs = (job[0]->getHashrate() + job[1]->getHashrate()) / 1000.0;

  html += "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='10'>";
  html += "<title>ESP32 Miner - Internet Sharing</title>";
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
  html += "<h1>⛏️ ESP32 Duino-Coin Miner + Internet Sharing</h1>";

  // Internet Sharing Status Card
  html += "<div class='card'>";
  html += "<h2>🌐 Internet Sharing Status</h2>";
  html += "<div class='info-box'>";
  html += "<strong>📡 Kết nối vào WiFi ESP32 để có internet (sau khi cấu hình WiFi nhà)</strong><br>";
  html += "</div>";
  html += "<table>";
  html += "<tr><th>WiFi STA (Internet)</th><td>";
  html += (wifiState == WIFI_CONNECTED) ? "<span class='status online'>Connected</span>" : "<span class='status offline'>Disconnected</span>";
  html += "</td></tr>";
  html += "<tr><th>Internet Access</th><td>";
  html += internetOK ? "<span class='status online'>Available</span>" : "<span class='status offline'>No</span>";
  html += "</td></tr>";
  html += "<tr><th>NAT (Sharing)</th><td>";
  html += natEnabled ? "<span class='status nat-enabled'>✓ Active - Internet is shared to AP clients</span>" : "<span class='status offline'>✗ Inactive</span>";
  html += "</td></tr>";
  html += "<tr><th>Connected Clients</th><td>" + String(WiFi.softAPgetStationNum()) + " device(s) can access internet</td></tr>";
  html += "</table>";
  html += "</div>";

  // Mining Status Card
  html += "<div class='card'>";
  html += "<h2>⛏️ Mining Status</h2>";
  html += "<table>";
  html += "<tr><th>Hashrate</th><td><span class='value'>" + String(hashrate_khs, 2) + " kH/s</span></td></tr>";
  html += "<tr><th>Shares</th><td>" + String(share_count) + " (Accepted: " + String(accepted_share_count) + ")</td></tr>";
  html += "<tr><th>Difficulty</th><td>" + String(difficulty / 100) + "</td></tr>";
  html += "<tr><th>Node</th><td>" + node_id + "</td></tr>";
  html += "<tr><th>Ping</th><td>" + String(ping) + " ms</td></tr>";
  html += "</table>";
  html += "</div>";

  html += "<div class='grid-2'>";

  // Station Configuration Card
  html += "<div class='card'>";
  html += "<h2>📶 WiFi Configuration (Internet Source)</h2>";
  html += "<p><small>Cấu hình WiFi nhà để ESP32 có internet và CHIA SẺ qua AP</small></p>";
  html += "<form method='POST' action='/save-sta'>";
  html += "<input type='text' name='ssid' placeholder='WiFi Name (SSID)' required>";
  html += "<input type='password' name='pass' placeholder='Password'>";
  html += "<button type='submit'>💾 Save & Connect</button>";
  html += "</form>";
  html += "<p><a href='/scan'><button type='button'>🔍 Scan Networks</button></a></p>";
  if (wifiState == WIFI_CONNECTED) {
    html += "<p style='margin-top:10px;color:#0f0;'>✓ Connected to: " + sta_ssid + "</p>";
  }
  html += "</div>";

  // AP Configuration Card
  html += "<div class='card'>";
  html += "<h2>📱 Access Point (WiFi để kết nối)</h2>";
  html += "<p><small>Đây là WiFi mà điện thoại/laptop sẽ kết nối vào để có internet</small></p>";
  html += "<form method='POST' action='/save-ap'>";
  html += "<input type='text' name='ssid' placeholder='AP SSID' value='" + ap_ssid + "' required>";
  html += "<input type='text' name='pass' placeholder='AP Password' value='" + ap_pass + "'>";
  html += "<button type='submit'>💾 Save AP Config</button>";
  html += "</form>";
  html += "<p style='margin-top:10px;'>🌐 Sau khi cấu hình WiFi nhà, kết nối vào <strong>" + ap_ssid + "</strong> để có internet!</p>";
  html += "</div>";

  html += "</div>";

  // System Status Card
  html += "<div class='card'>";
  html += "<h2>📊 System Info</h2>";
  html += "<table>";
  html += "<tr><th>AP SSID</th><td>" + ap_ssid + "</td></tr>";
  html += "<tr><th>AP IP</th><td>" + AP_IP.toString() + "</td></tr>";
  html += "<tr><th>STA SSID</th><td>" + (sta_ssid.length() ? sta_ssid : "Not configured") + "</td></tr>";
  html += "<tr><th>STA IP</th><td>" + (wifiState == WIFI_CONNECTED ? WiFi.localIP().toString() : "Not connected") + "</td></tr>";
  html += "<tr><th>Uptime</th><td>" + String(uptime) + "s</td></tr>";
  html += "<tr><th>Free Heap</th><td>" + String(ESP.getFreeHeap() / 1024) + " KB</td></tr>";
  html += "</table>";
  html += "</div>";

  // Action Buttons
  html += "<div class='card'>";
  html += "<h2>⚙️ Actions</h2>";
  html += "<a href='/mining'><button>📊 Mining JSON</button></a> ";
  html += "<a href='/nat-status'><button>🌐 NAT Status</button></a> ";
  html += "<a href='/reset'><button onclick='return confirm(\"Reset all settings?\")'>🗑️ Factory Reset</button></a> ";
  html += "<a href='/reboot'><button onclick='return confirm(\"Reboot ESP32?\")'>🔄 Reboot</button></a>";
  html += "</div>";

  html += "<div class='footer'>";
  html += "<p>ESP32 Duino-Coin Miner with Internet Sharing | " + String(__DATE__) + " " + String(__TIME__) + "</p>";
  html += "<p>📱 Kết nối vào AP: " + ap_ssid + " | 🌐 Internet sẽ được chia sẻ sau khi cấu hình WiFi nhà</p>";
  html += "<p>🔗 AP IP: " + AP_IP.toString() + " | mDNS: http://esp32miner.local</p>";
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

    String response = "<h1>✅ Saved!</h1>";
    response += "<p>Connecting to: " + new_ssid + "</p>";
    response += "<p>🌐 Sau khi kết nối thành công, internet sẽ được chia sẻ qua AP!</p>";
    response += "<a href='/'>Back</a>";
    server.send(200, "text/html", response);
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

    // Cập nhật AP ngay lập tức
    WiFi.softAP(ap_ssid.c_str(), ap_pass.c_str());

    String response = "<h1>✅ AP Saved!</h1>";
    response += "<p>New AP: " + new_ap_ssid + "</p>";
    response += "<p>📱 Kết nối vào AP này để có internet (sau khi cấu hình WiFi nhà)</p>";
    response += "<a href='/'>Back</a>";
    server.send(200, "text/html", response);
  } else {
    server.send(400, "text/plain", "AP SSID required!");
  }
}

void handleScan() {
  String html;
  html.reserve(4000);
  html += "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;padding:20px;background:#1a1a2e;color:white;} li{cursor:pointer;margin:5px 0;padding:8px;background:#16213e;border-radius:8px;} li:hover{background:#00d4ff22;}</style>";
  html += "<script>";
  html += "function pick(ssid){document.getElementById('ssid').value=ssid;alert('Selected: '+ssid);}";
  html += "</script>";
  html += "</head><body>";
  html += "<h2>📡 WiFi Networks</h2>";
  html += "<p>Click vào WiFi để chọn:</p><ul>";

  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    html += "<li onclick=\"pick('" + WiFi.SSID(i) + "')\">";
    html += WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + " dBm)";
    html += "</li>";
  }
  html += "</ul>";
  html += "<form method='POST' action='/save-sta'>";
  html += "<input type='text' id='ssid' name='ssid' placeholder='Or enter manually'>";
  html += "<input type='password' name='pass' placeholder='Password'>";
  html += "<button type='submit'>💾 Save & Connect</button>";
  html += "</form>";
  html += "<a href='/'><button>← Back</button></a>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleMiningStatus() {
  String json = "{";
  json += "\"hashrate_khs\":" + String((job[0]->getHashrate() + job[1]->getHashrate()) / 1000.0, 2) + ",";
  json += "\"shares\":" + String(share_count) + ",";
  json += "\"accepted_shares\":" + String(accepted_share_count) + ",";
  json += "\"difficulty\":" + String(difficulty / 100) + ",";
  json += "\"node\":\"" + node_id + "\",";
  json += "\"ping\":" + String(ping);
  json += "}";
  server.send(200, "application/json", json);
}

void handleStatus() {
  String json = "{";
  json += "\"wifi\":\"" + String((wifiState == WIFI_CONNECTED) ? "connected" : "disconnected") + "\",";
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
  server.send(200, "text/html", "<h1>Reset done!</h1><p>ESP32 will restart...</p>");
  delay(1000);
  ESP.restart();
}

void handleReboot() {
  disableNAT();
  server.send(200, "text/html", "<h1>Rebooting...</h1>");
  delay(1000);
  ESP.restart();
}
