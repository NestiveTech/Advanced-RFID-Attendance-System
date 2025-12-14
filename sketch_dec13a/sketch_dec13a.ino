/*
 * ═══════════════════════════════════════════════════════════════════════════
 * ENTERPRISE RFID ATTENDANCE SYSTEM - ESP32 PRODUCTION FIRMWARE v4.2
 * AUTO WIFI SCAN + MAX 5 NETWORKS STORAGE + IMMEDIATE AP MODE FIX
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_task_wdt.h>

// ═══════════════════════════════════════════════════════════════════════════
// CONFIGURATION - REPLACE WITH YOUR BACKEND URL
// ═══════════════════════════════════════════════════════════════════════════

const char* BACKEND_URL = "https://script.google.com/macros/s/AKfycbw76bDnTASZHJLQqZmEFnWJ_wB4b6gFeEwFuUR41N8W29jig5aGvVKe6uQv0uuBu4m5/exec";

// ═══════════════════════════════════════════════════════════════════════════
// PIN DEFINITIONS
// ═══════════════════════════════════════════════════════════════════════════

#define SS_PIN      5
#define RST_PIN     27
#define BUZZER_PIN  12
#define LED_PIN     2

// ═══════════════════════════════════════════════════════════════════════════
// FIRMWARE VERSION
// ═══════════════════════════════════════════════════════════════════════════

const char* FIRMWARE_VERSION = "4.2.0";

// ═══════════════════════════════════════════════════════════════════════════
// TIMING CONSTANTS
// ═══════════════════════════════════════════════════════════════════════════

#define WDT_TIMEOUT 30
#define WIFI_TIMEOUT 20000
#define AUTO_SCAN_INTERVAL 10000
#define HEARTBEAT_INTERVAL 60000
#define CONFIG_CHECK_INTERVAL 300000
#define RFID_COOLDOWN 3000
#define MAX_WIFI_NETWORKS 5

// ═══════════════════════════════════════════════════════════════════════════
// GLOBAL VARIABLES
// ═══════════════════════════════════════════════════════════════════════════

String DEVICE_ID = "";
String DEVICE_TOKEN = "";
String tenantId = "";
String deviceLocation = "";
bool deviceApproved = false;
bool silentMode = false;
bool registrationMode = false;

struct WiFiCred {
  String ssid;
  String password;
};

WiFiCred wifiNetworks[MAX_WIFI_NETWORKS];
int networkCount = 0;

String lastScannedUID = "";
unsigned long lastScanTime = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastConfigCheck = 0;
unsigned long lastWifiScan = 0;

bool isConfigPortalActive = false;
bool autoScanEnabled = true;

// ═══════════════════════════════════════════════════════════════════════════
// HARDWARE INSTANCES
// ═══════════════════════════════════════════════════════════════════════════

MFRC522 rfid(SS_PIN, RST_PIN);
Preferences prefs;
WebServer server(80);
WiFiMulti wifiMulti;

// ═══════════════════════════════════════════════════════════════════════════
// BUZZER PATTERNS
// ═══════════════════════════════════════════════════════════════════════════

enum BuzzerPattern {
  BUZZER_NONE,
  BUZZER_SUCCESS,
  BUZZER_ERROR,
  BUZZER_STARTUP
};

struct {
  BuzzerPattern pattern;
  unsigned long startTime;
  bool active;
} buzzer = {BUZZER_NONE, 0, false};

struct WiFiScanResult {
  String ssid;
  int rssi;
  bool encrypted;
};

WiFiScanResult cachedNetworks[20];
int cachedNetworkCount = 0;
unsigned long lastCacheUpdate = 0;

// ═══════════════════════════════════════════════════════════════════════════
// FUNCTION PROTOTYPES
// ═══════════════════════════════════════════════════════════════════════════

void loadConfig();
void saveConfig();
void startConfigPortal();
void handleRoot();
void handleScan();
void handleSave();
void handleStatus();
void handleRestart();
void handleDelete();
void handleFactoryReset();
bool connectWiFi();
bool registerDevice();
bool getDeviceConfig();
bool sendHeartbeat();
void handleRFID();
bool logAttendance(String uid);
bool registerRfidCard(String uid);
void playBuzzer(BuzzerPattern pattern);
void updateBuzzer();
void blinkLED(int times);
void autoScanWiFi();
void updateCachedNetworks();
String padString(String str, int length);

// ═══════════════════════════════════════════════════════════════════════════
// SETUP - FIXED VERSION WITH IMMEDIATE AP MODE
// ═══════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n╔═══════════════════════════════════════════════════╗");
  Serial.println("║   RFID ATTENDANCE SYSTEM v4.2 - FIXED AP MODE   ║");
  Serial.println("╚═══════════════════════════════════════════════════╝\n");
  
  // Initialize watchdog
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT * 1000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);
  
  // Initialize GPIO
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  
  // Generate device credentials
  uint64_t mac = ESP.getEfuseMac();
  DEVICE_ID = "ESP32-" + String((uint32_t)mac, HEX);
  DEVICE_ID.toUpperCase();
  DEVICE_TOKEN = "TOKEN-" + String((uint32_t)(mac >> 32), HEX);
  DEVICE_TOKEN.toUpperCase();
  
  Serial.println("📱 Device ID: " + DEVICE_ID);
  Serial.println("📦 Firmware: v" + String(FIRMWARE_VERSION));
  Serial.println("📶 Max Networks: " + String(MAX_WIFI_NETWORKS));
  
  // Initialize SPI and RFID
  SPI.begin();
  rfid.PCD_Init();
  
  byte version = rfid.PCD_ReadRegister(rfid.VersionReg);
  if (version == 0x00 || version == 0xFF) {
    Serial.println("❌ WARNING: RFID reader not detected!");
  } else {
    Serial.println("✅ RFID Reader: v0x" + String(version, HEX));
  }
  
  // Load configuration
  loadConfig();
  
  // Play startup sound
  playBuzzer(BUZZER_STARTUP);
  
  // ═══════════════════════════════════════════════════════════════════════
  // DECISION LOGIC: AP MODE vs WIFI CONNECTION
  // ═══════════════════════════════════════════════════════════════════════
  
  if (networkCount == 0) {
    // NO NETWORKS CONFIGURED - START AP MODE IMMEDIATELY
    Serial.println("\n🔧 No WiFi configured - Starting AP mode...");
    startConfigPortal();
    Serial.println("✅ Config portal ready!");
    blinkLED(5);
  } else {
    // NETWORKS CONFIGURED - TRY TO CONNECT
    Serial.println("\n📶 " + String(networkCount) + " network(s) configured");
    Serial.println("📡 Attempting to connect...");
    
    if (connectWiFi()) {
      // WIFI CONNECTED SUCCESSFULLY
      Serial.println("✅ WiFi connected successfully!");
      registerDevice();
      getDeviceConfig();
      sendHeartbeat();
      Serial.println("✅ System ready - RFID scanning enabled!");
      blinkLED(3);
    } else {
      // WIFI CONNECTION FAILED - FALLBACK TO AP MODE
      Serial.println("❌ WiFi connection failed!");
      Serial.println("🔧 Starting fallback config portal...");
      startConfigPortal();
      Serial.println("✅ Config portal ready - Fix WiFi settings!");
      blinkLED(5);
    }
  }
  
  Serial.println("\n" + String('=', 55));
  Serial.println("DEVICE STATUS:");
  Serial.println("  Config Portal: " + String(isConfigPortalActive ? "ACTIVE ✓" : "Inactive"));
  Serial.println("  WiFi Status: " + String(WiFi.status() == WL_CONNECTED ? "Connected ✓" : "Disconnected"));
  Serial.println("  Device Approved: " + String(deviceApproved ? "Yes ✓" : "No"));
  Serial.println(String('=', 55) + "\n");
}

// ═══════════════════════════════════════════════════════════════════════════
// MAIN LOOP
// ═══════════════════════════════════════════════════════════════════════════

void loop() {
  unsigned long currentMillis = millis();
  
  // Feed watchdog
  esp_task_wdt_reset();
  
  // Handle web server if active
  if (isConfigPortalActive) {
    server.handleClient();
  }
  
  // Update non-blocking buzzer
  updateBuzzer();
  
  // Auto-scan WiFi when config portal is active
  if (isConfigPortalActive && autoScanEnabled) {
    if (currentMillis - lastWifiScan > AUTO_SCAN_INTERVAL) {
      autoScanWiFi();
      lastWifiScan = currentMillis;
    }
  }
  
  // If not connected to WiFi, don't process RFID
  if (WiFi.status() != WL_CONNECTED && !isConfigPortalActive) {
    if (currentMillis - lastScanTime > 30000) {
      Serial.println("⚠️  WiFi disconnected, attempting reconnect...");
      connectWiFi();
      lastScanTime = currentMillis;
    }
    delay(100);
    return;
  }
  
  // Handle RFID scanning (only when WiFi connected and not in config mode)
  if (WiFi.status() == WL_CONNECTED && !isConfigPortalActive) {
    handleRFID();
    
    // Send heartbeat
    if (currentMillis - lastHeartbeat > HEARTBEAT_INTERVAL) {
      sendHeartbeat();
      lastHeartbeat = currentMillis;
    }
    
    // Check config updates
    if (currentMillis - lastConfigCheck > CONFIG_CHECK_INTERVAL) {
      getDeviceConfig();
      lastConfigCheck = currentMillis;
    }
  }
  
  yield();
}

// ═══════════════════════════════════════════════════════════════════════════
// AUTO WIFI SCAN
// ═══════════════════════════════════════════════════════════════════════════

void autoScanWiFi() {
  Serial.println("🔍 Auto-scanning WiFi networks...");
  updateCachedNetworks();
}

void updateCachedNetworks() {
  WiFi.mode(WIFI_AP_STA);
  int n = WiFi.scanNetworks(false, true);
  
  if (n == WIFI_SCAN_RUNNING) {
    return;
  }
  
  cachedNetworkCount = 0;
  
  if (n > 0) {
    Serial.println("✅ Found " + String(n) + " networks");
    
    for (int i = 0; i < n && cachedNetworkCount < 20; i++) {
      String ssid = WiFi.SSID(i);
      
      if (ssid.length() == 0) continue;
      
      bool isDuplicate = false;
      for (int j = 0; j < cachedNetworkCount; j++) {
        if (cachedNetworks[j].ssid == ssid) {
          if (WiFi.RSSI(i) > cachedNetworks[j].rssi) {
            cachedNetworks[j].rssi = WiFi.RSSI(i);
          }
          isDuplicate = true;
          break;
        }
      }
      
      if (!isDuplicate) {
        cachedNetworks[cachedNetworkCount].ssid = ssid;
        cachedNetworks[cachedNetworkCount].rssi = WiFi.RSSI(i);
        cachedNetworks[cachedNetworkCount].encrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        cachedNetworkCount++;
      }
    }
    
    // Sort by signal strength
    for (int i = 0; i < cachedNetworkCount - 1; i++) {
      for (int j = i + 1; j < cachedNetworkCount; j++) {
        if (cachedNetworks[j].rssi > cachedNetworks[i].rssi) {
          WiFiScanResult temp = cachedNetworks[i];
          cachedNetworks[i] = cachedNetworks[j];
          cachedNetworks[j] = temp;
        }
      }
    }
    
    lastCacheUpdate = millis();
  }
  
  WiFi.scanDelete();
}

// ═══════════════════════════════════════════════════════════════════════════
// CONFIGURATION MANAGEMENT
// ═══════════════════════════════════════════════════════════════════════════

void loadConfig() {
  prefs.begin("rfid-system", false);
  
  networkCount = prefs.getInt("wifi_count", 0);
  
  if (networkCount > MAX_WIFI_NETWORKS) {
    networkCount = MAX_WIFI_NETWORKS;
  }
  
  Serial.println("\n📂 Loading configuration:");
  Serial.println("   WiFi Networks: " + String(networkCount) + "/" + String(MAX_WIFI_NETWORKS));
  
  for (int i = 0; i < networkCount; i++) {
    String ssidKey = "ssid_" + String(i);
    String passKey = "pass_" + String(i);
    
    wifiNetworks[i].ssid = prefs.getString(ssidKey.c_str(), "");
    wifiNetworks[i].password = prefs.getString(passKey.c_str(), "");
    
    if (wifiNetworks[i].ssid.length() > 0) {
      Serial.println("   [" + String(i + 1) + "] " + wifiNetworks[i].ssid);
      wifiMulti.addAP(wifiNetworks[i].ssid.c_str(), wifiNetworks[i].password.c_str());
    }
  }
  
  tenantId = prefs.getString("tenant_id", "");
  deviceLocation = prefs.getString("location", "Unassigned");
  deviceApproved = prefs.getBool("approved", false);
  silentMode = prefs.getBool("silent", false);
  registrationMode = prefs.getBool("reg_mode", false);
  
  Serial.println("   Tenant ID: " + (tenantId.length() > 0 ? tenantId : "None"));
  Serial.println("   Approved: " + String(deviceApproved ? "Yes" : "No"));
  
  prefs.end();
}

void saveConfig() {
  prefs.begin("rfid-system", false);
  
  prefs.putInt("wifi_count", networkCount);
  
  for (int i = 0; i < networkCount; i++) {
    String ssidKey = "ssid_" + String(i);
    String passKey = "pass_" + String(i);
    prefs.putString(ssidKey.c_str(), wifiNetworks[i].ssid);
    prefs.putString(passKey.c_str(), wifiNetworks[i].password);
  }
  
  prefs.putString("tenant_id", tenantId);
  prefs.putString("location", deviceLocation);
  prefs.putBool("approved", deviceApproved);
  prefs.putBool("silent", silentMode);
  prefs.putBool("reg_mode", registrationMode);
  
  prefs.end();
  
  Serial.println("💾 Configuration saved!");
}

// ═══════════════════════════════════════════════════════════════════════════
// WIFI CONNECTION - IMPROVED WITH BETTER TIMEOUT HANDLING
// ═══════════════════════════════════════════════════════════════════════════

bool connectWiFi() {
  Serial.println("\n📶 Starting WiFi connection...");
  Serial.println("   Timeout: " + String(WIFI_TIMEOUT / 1000) + " seconds");
  
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  
  // Show all configured networks
  Serial.println("\n📋 Configured Networks:");
  for (int i = 0; i < networkCount; i++) {
    Serial.println("   [" + String(i + 1) + "] " + wifiNetworks[i].ssid);
  }
  
  unsigned long startTime = millis();
  uint8_t attempt = 0;
  
  while (millis() - startTime < WIFI_TIMEOUT) {
    uint8_t status = wifiMulti.run();
    
    if (status == WL_CONNECTED) {
      Serial.println("\n✅ CONNECTED!");
      Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
      Serial.println("  Network: " + WiFi.SSID());
      Serial.println("  IP Address: " + WiFi.localIP().toString());
      Serial.println("  Signal Strength: " + String(WiFi.RSSI()) + " dBm");
      Serial.println("  MAC Address: " + WiFi.macAddress());
      Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
      return true;
    }
    
    // Visual feedback every 2 seconds
    if ((millis() - startTime) / 2000 > attempt) {
      attempt++;
      Serial.print(".");
      digitalWrite(LED_PIN, !digitalRead(LED_PIN)); // Blink LED
    }
    
    delay(100);
    esp_task_wdt_reset(); // Feed watchdog during connection
  }
  
  Serial.println("\n❌ CONNECTION TIMEOUT!");
  Serial.println("   Elapsed: " + String((millis() - startTime) / 1000) + " seconds");
  digitalWrite(LED_PIN, LOW);
  return false;
}

// ═══════════════════════════════════════════════════════════════════════════
// IMPROVED CONFIG PORTAL WITH BETTER ERROR HANDLING
// ═══════════════════════════════════════════════════════════════════════════

String padString(String str, int length) {
  while (str.length() < length) {
    str += " ";
  }
  return str;
}

void startConfigPortal() {
  isConfigPortalActive = true;
  
  // Stop any existing WiFi connection
  WiFi.disconnect();
  delay(100);
  
  String apSSID = "RFID-Setup-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  apSSID.toUpperCase();
  String apPassword = "12345678";
  
  // Set WiFi mode to AP+STA (Access Point + Station)
  WiFi.mode(WIFI_AP_STA);
  delay(100);
  
  // Configure and start Access Point
  WiFi.softAPConfig(
    IPAddress(192, 168, 4, 1),  // AP IP
    IPAddress(192, 168, 4, 1),  // Gateway
    IPAddress(255, 255, 255, 0) // Subnet
  );
  
  bool apStarted = WiFi.softAP(apSSID.c_str(), apPassword.c_str(), 1, 0, 4);
  
  if (!apStarted) {
    Serial.println("❌ Failed to start AP! Retrying...");
    delay(1000);
    apStarted = WiFi.softAP(apSSID.c_str(), apPassword.c_str());
  }
  
  if (apStarted) {
    IPAddress IP = WiFi.softAPIP();
    
    Serial.println("\n╔════════════════════════════════════════════════════╗");
    Serial.println("║         🔧 CONFIGURATION PORTAL ACTIVE 🔧          ║");
    Serial.println("╠════════════════════════════════════════════════════╣");
    Serial.println("║                                                    ║");
    Serial.println("║  STEP 1: Connect to WiFi Network                  ║");
    Serial.println("║  ────────────────────────────────────────────      ║");
    Serial.println("║  SSID:     " + padString(apSSID, 36) + " ║");
    Serial.println("║  Password: " + padString(apPassword, 36) + " ║");
    Serial.println("║                                                    ║");
    Serial.println("║  STEP 2: Open Browser                             ║");
    Serial.println("║  ────────────────────────────────────────────      ║");
    Serial.println("║  URL: http://192.168.4.1                          ║");
    Serial.println("║                                                    ║");
    Serial.println("║  STEP 3: Configure Your WiFi                      ║");
    Serial.println("║  ────────────────────────────────────────────      ║");
    Serial.println("║  → Select WiFi network from list                  ║");
    Serial.println("║  → Enter WiFi password                            ║");
    Serial.println("║  → Click 'Save & Connect'                         ║");
    Serial.println("║                                                    ║");
    Serial.println("╠════════════════════════════════════════════════════╣");
    Serial.println("║  Features:                                         ║");
    Serial.println("║  ✓ Store up to " + String(MAX_WIFI_NETWORKS) + " WiFi networks                   ║");
    Serial.println("║  ✓ Auto WiFi scan every 10 seconds                ║");
    Serial.println("║  ✓ Signal strength indicator                      ║");
    Serial.println("║  ✓ Delete/manage saved networks                   ║");
    Serial.println("╚════════════════════════════════════════════════════╝\n");
    
    // Setup web server routes
    server.on("/", handleRoot);
    server.on("/scan", handleScan);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/status", handleStatus);
    server.on("/restart", handleRestart);
    server.on("/delete", HTTP_POST, handleDelete);
    server.on("/factory-reset", HTTP_POST, handleFactoryReset);
    
    // Handle 404
    server.onNotFound([]() {
      server.sendHeader("Location", "/", true);
      server.send(302, "text/plain", "");
    });
    
    server.begin();
    Serial.println("✅ Web server started successfully!");
    Serial.println("🔍 Auto WiFi scan: ENABLED (every 10 seconds)");
    
    // Initial WiFi scan
    updateCachedNetworks();
    lastWifiScan = millis();
    
  } else {
    Serial.println("❌ CRITICAL: Failed to start Access Point!");
    Serial.println("   Device will restart in 10 seconds...");
    delay(10000);
    ESP.restart();
  }
}

// Continue in next part due to length...
// ═══════════════════════════════════════════════════════════════════════════
// WEB SERVER HANDLERS
// ═══════════════════════════════════════════════════════════════════════════

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>RFID WiFi Setup</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      min-height: 100vh;
      padding: 20px;
    }
    .container {
      background: white;
      max-width: 500px;
      margin: 0 auto;
      padding: 30px;
      border-radius: 20px;
      box-shadow: 0 20px 60px rgba(0,0,0,0.3);
    }
    h1 {
      color: #333;
      text-align: center;
      margin-bottom: 10px;
      font-size: 24px;
    }
    .device-id {
      background: linear-gradient(135deg, #667eea, #764ba2);
      color: white;
      padding: 15px;
      margin: 20px 0;
      text-align: center;
      border-radius: 12px;
      font-weight: 600;
      letter-spacing: 1px;
    }
    .network-limit {
      background: #fff3cd;
      color: #856404;
      padding: 12px;
      border-radius: 10px;
      margin-bottom: 20px;
      text-align: center;
      font-size: 14px;
      font-weight: 600;
      border: 2px solid #ffc107;
    }
    .saved-networks {
      margin-bottom: 25px;
    }
    .saved-header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 15px;
    }
    .saved-header h3 {
      color: #333;
      font-size: 16px;
    }
    .network-count {
      background: #667eea;
      color: white;
      padding: 5px 12px;
      border-radius: 20px;
      font-size: 13px;
      font-weight: 700;
    }
    .network-item {
      background: #f8f9fa;
      border: 2px solid #e9ecef;
      padding: 15px;
      margin-bottom: 10px;
      border-radius: 12px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      transition: all 0.3s;
    }
    .network-item:hover {
      border-color: #667eea;
      transform: translateX(5px);
    }
    .network-name {
      font-weight: 600;
      color: #333;
      display: flex;
      align-items: center;
      gap: 10px;
    }
    .wifi-icon {
      font-size: 20px;
    }
    .btn-delete {
      background: #dc3545;
      color: white;
      border: none;
      padding: 8px 15px;
      border-radius: 8px;
      cursor: pointer;
      font-size: 12px;
      font-weight: 600;
      transition: all 0.3s;
    }
    .btn-delete:hover {
      background: #c82333;
      transform: scale(1.05);
    }
    .wifi-list {
      margin-bottom: 20px;
    }
    .wifi-header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 15px;
    }
    .wifi-header h3 {
      color: #333;
      font-size: 16px;
    }
    .auto-scan-badge {
      background: #28a745;
      color: white;
      padding: 5px 12px;
      border-radius: 20px;
      font-size: 11px;
      font-weight: 700;
      display: flex;
      align-items: center;
      gap: 5px;
      animation: pulse 2s infinite;
    }
    @keyframes pulse {
      0%, 100% { opacity: 1; }
      50% { opacity: 0.7; }
    }
    .wifi-networks {
      max-height: 300px;
      overflow-y: auto;
      border: 2px solid #e9ecef;
      border-radius: 12px;
    }
    .wifi-network {
      padding: 15px;
      border-bottom: 1px solid #f0f0f0;
      cursor: pointer;
      display: flex;
      justify-content: space-between;
      align-items: center;
      transition: all 0.3s;
    }
    .wifi-network:last-child {
      border-bottom: none;
    }
    .wifi-network:hover {
      background: #f8f9fa;
    }
    .wifi-network.selected {
      background: linear-gradient(135deg, rgba(102, 126, 234, 0.1), rgba(118, 75, 162, 0.1));
      border-left: 4px solid #667eea;
    }
    .wifi-info {
      display: flex;
      align-items: center;
      gap: 12px;
    }
    .signal-strength {
      width: 40px;
      text-align: center;
    }
    .signal-bars {
      display: inline-flex;
      gap: 2px;
      align-items: flex-end;
      height: 20px;
    }
    .bar {
      width: 4px;
      background: #ccc;
      border-radius: 2px;
    }
    .bar.active {
      background: #28a745;
    }
    .form-group {
      margin-bottom: 20px;
    }
    .form-group label {
      display: block;
      margin-bottom: 8px;
      font-weight: 600;
      color: #333;
      font-size: 14px;
    }
    input[type="text"],
    input[type="password"] {
      width: 100%;
      padding: 14px;
      border: 2px solid #e0e0e0;
      border-radius: 10px;
      font-size: 15px;
      transition: all 0.3s;
    }
    input:focus {
      outline: none;
      border-color: #667eea;
      box-shadow: 0 0 0 3px rgba(102, 126, 234, 0.1);
    }
    .btn {
      width: 100%;
      padding: 15px;
      border: none;
      border-radius: 10px;
      font-size: 16px;
      font-weight: 600;
      cursor: pointer;
      transition: all 0.3s;
      margin-top: 10px;
    }
    .btn-primary {
      background: linear-gradient(135deg, #667eea, #764ba2);
      color: white;
    }
    .btn-primary:hover {
      transform: translateY(-2px);
      box-shadow: 0 10px 25px rgba(102, 126, 234, 0.3);
    }
    .btn-success {
      background: linear-gradient(135deg, #28a745, #20c997);
      color: white;
    }
    .btn-success:hover {
      transform: translateY(-2px);
      box-shadow: 0 10px 25px rgba(40, 167, 69, 0.3);
    }
    .btn-danger {
      background: linear-gradient(135deg, #dc3545, #c82333);
      color: white;
    }
    .btn-danger:hover {
      transform: translateY(-2px);
      box-shadow: 0 10px 25px rgba(220, 53, 69, 0.3);
    }
    .btn:disabled {
      opacity: 0.5;
      cursor: not-allowed;
      transform: none !important;
    }
    .alert {
      padding: 12px;
      border-radius: 8px;
      margin-bottom: 15px;
      display: none;
    }
    .alert-success {
      background: #d4edda;
      color: #155724;
      border: 1px solid #c3e6cb;
    }
    .alert-error {
      background: #f8d7da;
      color: #721c24;
      border: 1px solid #f5c6cb;
    }
    .alert-warning {
      background: #fff3cd;
      color: #856404;
      border: 1px solid #ffeaa7;
    }
    .loading {
      text-align: center;
      padding: 30px;
      color: #999;
    }
    .spinner {
      border: 3px solid #f3f3f3;
      border-top: 3px solid #667eea;
      border-radius: 50%;
      width: 40px;
      height: 40px;
      animation: spin 1s linear infinite;
      margin: 0 auto 15px;
    }
    @keyframes spin {
      0% { transform: rotate(0deg); }
      100% { transform: rotate(360deg); }
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>📶 WiFi Setup</h1>
    
    <div class="device-id">
      <div style="font-size: 11px; opacity: 0.9; margin-bottom: 5px;">DEVICE ID</div>
      <div style="font-size: 16px;">)rawliteral" + DEVICE_ID + R"rawliteral(</div>
    </div>
    
    <div class="network-limit">
      ⚠️ Maximum )rawliteral" + String(MAX_WIFI_NETWORKS) + R"rawliteral( networks can be stored
    </div>
    
    <div id="alert" class="alert"></div>
    
    <div class="saved-networks">
      <div class="saved-header">
        <h3>💾 Saved Networks</h3>
        <span class="network-count" id="networkCount">0/)rawliteral" + String(MAX_WIFI_NETWORKS) + R"rawliteral(</span>
      </div>
      <div id="savedNetworks"></div>
    </div>
    
    <div class="wifi-list">
      <div class="wifi-header">
        <h3>📡 Available Networks</h3>
        <span class="auto-scan-badge">
          <span style="animation: pulse 1s infinite;">●</span>
          AUTO-SCAN
        </span>
      </div>
      <div id="wifiNetworks" class="wifi-networks">
        <div class="loading">
          <div class="spinner"></div>
          <div>Scanning for networks...</div>
        </div>
      </div>
    </div>
    
    <form id="wifiForm" onsubmit="saveNetwork(event)">
      <div class="form-group">
        <label>WiFi Name (SSID)</label>
        <input type="text" id="ssid" placeholder="Select from list or type manually" required>
      </div>
      
      <div class="form-group">
        <label>WiFi Password</label>
        <input type="password" id="password" placeholder="Enter password" required>
      </div>
      
      <button type="submit" class="btn btn-primary" id="addBtn">
        ➕ Add Network
      </button>
      
      <button type="button" class="btn btn-success" onclick="saveAndConnect()">
        🚀 Save & Connect
      </button>
      
      <button type="button" class="btn btn-danger" onclick="factoryReset()">
        🗑️ Factory Reset
      </button>
    </form>
  </div>
  
  <script>
    let selectedSSID = '';
    let savedCount = 0;
    
    function showAlert(type, message) {
      const alert = document.getElementById('alert');
      alert.className = 'alert alert-' + type;
      alert.innerHTML = message;
      alert.style.display = 'block';
      setTimeout(() => alert.style.display = 'none', 5000);
    }
    
    function getSignalBars(rssi) {
      let bars = '';
      let activeCount = 0;
      
      if (rssi >= -50) activeCount = 4;
      else if (rssi >= -60) activeCount = 3;
      else if (rssi >= -70) activeCount = 2;
      else activeCount = 1;
      
      for (let i = 1; i <= 4; i++) {
        const height = i * 5;
        const active = i <= activeCount ? 'active' : '';
        bars += `<div class="bar ${active}" style="height: ${height}px;"></div>`;
      }
      
      return `<div class="signal-bars">${bars}</div>`;
    }
    
    function selectNetwork(ssid) {
      selectedSSID = ssid;
      document.getElementById('ssid').value = ssid;
      
      document.querySelectorAll('.wifi-network').forEach(net => {
        net.classList.remove('selected');
      });
      
      event.currentTarget.classList.add('selected');
      showAlert('success', '✅ Selected: ' + ssid);
    }
    
    async function loadNetworks() {
      try {
        const res = await fetch('/scan');
        const data = await res.json();
        
        const container = document.getElementById('wifiNetworks');
        
        if (data.networks && data.networks.length > 0) {
          container.innerHTML = data.networks.map(net => `
            <div class="wifi-network" onclick="selectNetwork('${net.ssid}')">
              <div class="wifi-info">
                <div class="signal-strength">
                  ${getSignalBars(net.rssi)}
                </div>
                <div>
                  <div style="font-weight: 600; color: #333;">${net.ssid}</div>
                  <div style="font-size: 12px; color: #999;">${net.rssi} dBm ${net.encrypted ? '🔒' : '🔓'}</div>
                </div>
              </div>
              <span style="color: #667eea; font-size: 20px;">›</span>
            </div>
          `).join('');
        } else {
          container.innerHTML = '<div class="loading">No networks found</div>';
        }
      } catch (error) {
        console.error('Error loading networks:', error);
      }
    }
    
    async function loadStatus() {
      try {
        const res = await fetch('/status');
        const data = await res.json();
        
        savedCount = data.network_count || 0;
        document.getElementById('networkCount').textContent = savedCount + '/)rawliteral" + String(MAX_WIFI_NETWORKS) + R"rawliteral(';
        
        const container = document.getElementById('savedNetworks');
        
        if (savedCount === 0) {
          container.innerHTML = '<div style="text-align: center; padding: 20px; color: #999;">No saved networks</div>';
        } else {
          container.innerHTML = data.saved_networks.map((net, idx) => `
            <div class="network-item">
              <div class="network-name">
                <span class="wifi-icon">📶</span>
                <div>
                  <div style="font-weight: 600;">${net.ssid}</div>
                  <div style="font-size: 12px; color: #666;">Slot ${idx + 1}/)rawliteral" + String(MAX_WIFI_NETWORKS) + R"rawliteral(</div>
                </div>
              </div>
              <button class="btn-delete" onclick="deleteNetwork(${idx})">🗑️ Delete</button>
            </div>
          `).join('');
        }
        
        const addBtn = document.getElementById('addBtn');
        if (savedCount >= )rawliteral" + String(MAX_WIFI_NETWORKS) + R"rawliteral() {
          addBtn.disabled = true;
          addBtn.innerHTML = '⚠️ Maximum Networks Reached';
        } else {
          addBtn.disabled = false;
          addBtn.innerHTML = '➕ Add Network (' + savedCount + '/)rawliteral" + String(MAX_WIFI_NETWORKS) + R"rawliteral()';
        }
      } catch (error) {
        console.error('Error loading status:', error);
      }
    }
    
    async function saveNetwork(e) {
      e.preventDefault();
      
      if (savedCount >= )rawliteral" + String(MAX_WIFI_NETWORKS) + R"rawliteral() {
        showAlert('warning', '⚠️ Maximum )rawliteral" + String(MAX_WIFI_NETWORKS) + R"rawliteral( networks limit reached. Delete a network first.');
        return;
      }
      
      const ssid = document.getElementById('ssid').value;
      const password = document.getElementById('password').value;
      
      try {
        const res = await fetch('/save', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({ssid, password})
        });
        
        const data = await res.json();
        
        if (data.status === 'success') {
          showAlert('success', '✅ Network added successfully!');
          document.getElementById('password').value = '';
          loadStatus();
        } else {
          showAlert('error', '❌ ' + data.message);
        }
      } catch (error) {
        showAlert('error', '❌ Failed to save network');
      }
    }
    
    async function deleteNetwork(index) {
      if (!confirm('Delete this network?')) return;
      
      try {
        const res = await fetch('/delete', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({index: index})
        });
        
        const data = await res.json();
        
        if (data.status === 'success') {
          showAlert('success', '✅ Network deleted');
          loadStatus();
        } else {
          showAlert('error', '❌ ' + data.message);
        }
      } catch (error) {
        showAlert('error', '❌ Failed to delete network');
      }
    }
    
    async function saveAndConnect() {
      if (confirm('Save configuration and restart device?')) {
        try {
          await fetch('/restart', {method: 'POST'});
          showAlert('success', '✅ Device restarting...');
          setTimeout(() => {
            window.close();
          }, 3000);
        } catch (error) {
          showAlert('error', '❌ Restart failed');
        }
      }
    }
    
    async function factoryReset() {
      if (!confirm('⚠️ WARNING: This will delete ALL saved WiFi networks and reset the device.\n\nAre you sure?')) {
        return;
      }
      
      if (!confirm('This action CANNOT be undone. Continue?')) {
        return;
      }
      
      try {
        await fetch('/factory-reset', {method: 'POST'});
        showAlert('success', '✅ Factory reset complete. Device restarting...');
        setTimeout(() => {
          window.location.reload();
        }, 5000);
      } catch (error) {
        showAlert('error', '❌ Factory reset failed');
      }
    }
    
    setInterval(loadNetworks, 10000);
    loadNetworks();
    loadStatus();
    setInterval(loadStatus, 5000);
  </script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

void handleScan() {
  DynamicJsonDocument doc(4096);
  JsonArray networks = doc.createNestedArray("networks");
  
  for (int i = 0; i < cachedNetworkCount; i++) {
    JsonObject network = networks.createNestedObject();
    network["ssid"] = cachedNetworks[i].ssid;
    network["rssi"] = cachedNetworks[i].rssi;
    network["encrypted"] = cachedNetworks[i].encrypted;
  }
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleSave() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"No data\"}");
    return;
  }
  
  String body = server.arg("plain");
  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, body);
  
  if (error) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid JSON\"}");
    return;
  }
  
  String ssid = doc["ssid"].as<String>();
  String password = doc["password"].as<String>();
  
  if (ssid.length() == 0) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"SSID required\"}");
    return;
  }
  
  if (networkCount >= MAX_WIFI_NETWORKS) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Maximum " + String(MAX_WIFI_NETWORKS) + " networks limit reached\"}");
    return;
  }
  
  bool exists = false;
  for (int i = 0; i < networkCount; i++) {
    if (wifiNetworks[i].ssid == ssid) {
      wifiNetworks[i].password = password;
      exists = true;
      break;
    }
  }
  
  if (!exists) {
    wifiNetworks[networkCount].ssid = ssid;
    wifiNetworks[networkCount].password = password;
    networkCount++;
    wifiMulti.addAP(ssid.c_str(), password.c_str());
  }
  
  saveConfig();
  Serial.println("✅ WiFi saved: " + ssid + " (" + String(networkCount) + "/" + String(MAX_WIFI_NETWORKS) + ")");
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Network saved\"}");
}

void handleStatus() {
  DynamicJsonDocument doc(2048);
  doc["device_id"] = DEVICE_ID;
  doc["network_count"] = networkCount;
  doc["max_networks"] = MAX_WIFI_NETWORKS;
  doc["wifi_connected"] = (WiFi.status() == WL_CONNECTED);
  doc["approved"] = deviceApproved;
  doc["tenant_id"] = tenantId;
  
  JsonArray savedNetworks = doc.createNestedArray("saved_networks");
  for (int i = 0; i < networkCount; i++) {
    JsonObject net = savedNetworks.createNestedObject();
    net["ssid"] = wifiNetworks[i].ssid;
  }
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleDelete() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"No data\"}");
    return;
  }
  
  String body = server.arg("plain");
  DynamicJsonDocument doc(256);
  DeserializationError error = deserializeJson(doc, body);
  
  if (error) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid JSON\"}");
    return;
  }
  
  int index = doc["index"];
  
  if (index < 0 || index >= networkCount) {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid index\"}");
    return;
  }
  
  for (int i = index; i < networkCount - 1; i++) {
    wifiNetworks[i] = wifiNetworks[i + 1];
  }
  
  networkCount--;
  saveConfig();
  Serial.println("🗑️ Network deleted. Remaining: " + String(networkCount));
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Network deleted\"}");
}

void handleRestart() {
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Restarting...\"}");
  delay(1000);
  ESP.restart();
}

void handleFactoryReset() {
  server.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Factory reset initiated\"}");
  
  Serial.println("\n⚠️ FACTORY RESET INITIATED!");
  delay(1000);
  
  // Clear all saved data
  prefs.begin("rfid-system", false);
  prefs.clear();
  prefs.end();
  
  Serial.println("✅ All data cleared");
  Serial.println("🔄 Restarting device...");
  
  delay(2000);
  ESP.restart();
}

// ═══════════════════════════════════════════════════════════════════════════
// BACKEND API CALLS
// ═══════════════════════════════════════════════════════════════════════════

bool registerDevice() {
  if (WiFi.status() != WL_CONNECTED) return false;
  
  Serial.println("\n📝 Registering device with backend...");
  
  HTTPClient http;
  http.begin(BACKEND_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);
  
  DynamicJsonDocument doc(512);
  doc["action"] = "register_device";
  doc["device_id"] = DEVICE_ID;
  doc["device_token"] = DEVICE_TOKEN;
  doc["mac_address"] = WiFi.macAddress();
  doc["ip_address"] = WiFi.localIP().toString();
  doc["firmware_version"] = FIRMWARE_VERSION;
  
  String requestBody;
  serializeJson(doc, requestBody);
  
  int httpCode = http.POST(requestBody);
  
  if (httpCode == 200) {
    String response = http.getString();
    DynamicJsonDocument responseDoc(1024);
    deserializeJson(responseDoc, response);
    
    if (strcmp(responseDoc["status"], "success") == 0) {
      deviceApproved = responseDoc["approved"];
      Serial.println("✅ Device registered");
      Serial.println("   Approved: " + String(deviceApproved ? "Yes" : "No"));
      http.end();
      return true;
    }
  }
  
  Serial.println("❌ Registration failed");
  http.end();
  return false;
}

bool getDeviceConfig() {
  if (WiFi.status() != WL_CONNECTED) return false;
  
  HTTPClient http;
  http.begin(BACKEND_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);
  
  DynamicJsonDocument doc(512);
  doc["action"] = "get_device_config";
  doc["device_id"] = DEVICE_ID;
  doc["device_token"] = DEVICE_TOKEN;
  
  String requestBody;
  serializeJson(doc, requestBody);
  
  int httpCode = http.POST(requestBody);
  
  if (httpCode == 200) {
    String response = http.getString();
    DynamicJsonDocument responseDoc(1024);
    deserializeJson(responseDoc, response);
    
    if (strcmp(responseDoc["status"], "success") == 0) {
      deviceApproved = responseDoc["approved"];
      tenantId = responseDoc["tenant_id"].as<String>();
      deviceLocation = responseDoc["location"].as<String>();
      silentMode = responseDoc["silent_mode"];
      registrationMode = responseDoc["registration_mode"];
      
      saveConfig();
      Serial.println("✅ Config updated");
      http.end();
      return true;
    }
  }
  
  http.end();
  return false;
}

bool sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) return false;
  
  HTTPClient http;
  http.begin(BACKEND_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);
  
  DynamicJsonDocument doc(512);
  doc["action"] = "device_heartbeat";
  doc["device_id"] = DEVICE_ID;
  doc["device_token"] = DEVICE_TOKEN;
  
  String requestBody;
  serializeJson(doc, requestBody);
  
  int httpCode = http.POST(requestBody);
  bool success = (httpCode == 200);
  
  http.end();
  return success;
}

// ═══════════════════════════════════════════════════════════════════════════
// RFID SCANNING
// ═══════════════════════════════════════════════════════════════════════════

void handleRFID() {
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;
  
  unsigned long currentMillis = millis();
  
  if (currentMillis - lastScanTime < RFID_COOLDOWN) {
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return;
  }
  
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  
  if (uid == lastScannedUID && currentMillis - lastScanTime < 60000) {
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return;
  }
  
  lastScannedUID = uid;
  lastScanTime = currentMillis;
  
  Serial.println("\n🔍 RFID Scanned: " + uid);
  
  if (registrationMode) {
    if (registerRfidCard(uid)) {
      playBuzzer(BUZZER_SUCCESS);
      Serial.println("✅ Card registered for assignment");
      blinkLED(3);
    } else {
      playBuzzer(BUZZER_ERROR);
    }
  } else {
    if (logAttendance(uid)) {
      playBuzzer(BUZZER_SUCCESS);
      blinkLED(2);
    } else {
      playBuzzer(BUZZER_ERROR);
    }
  }
  
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

bool logAttendance(String uid) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!deviceApproved) {
    Serial.println("❌ Device not approved");
    return false;
  }
  
  HTTPClient http;
  http.begin(BACKEND_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);
  
  DynamicJsonDocument doc(512);
  doc["action"] = "log_attendance";
  doc["device_id"] = DEVICE_ID;
  doc["device_token"] = DEVICE_TOKEN;
  doc["rfid_uid"] = uid;
  
  String requestBody;
  serializeJson(doc, requestBody);
  
  int httpCode = http.POST(requestBody);
  
  if (httpCode == 200) {
    String response = http.getString();
    DynamicJsonDocument responseDoc(1024);
    deserializeJson(responseDoc, response);
    
    if (strcmp(responseDoc["status"], "success") == 0) {
      String employeeName = responseDoc["employee_name"].as<String>();
      String attendanceType = responseDoc["attendance_type"].as<String>();
      
      Serial.println("✅ Attendance logged:");
      Serial.println("   Employee: " + employeeName);
      Serial.println("   Type: " + attendanceType);
      
      http.end();
      return true;
    } else {
      Serial.println("❌ " + String(responseDoc["message"].as<const char*>()));
    }
  }
  
  http.end();
  return false;
}

bool registerRfidCard(String uid) {
  if (WiFi.status() != WL_CONNECTED) return false;
  
  HTTPClient http;
  http.begin(BACKEND_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);
  
  DynamicJsonDocument doc(512);
  doc["action"] = "register_rfid_card";
  doc["rfid_uid"] = uid;
  doc["device_id"] = DEVICE_ID;
  doc["device_token"] = DEVICE_TOKEN;
  
  String requestBody;
  serializeJson(doc, requestBody);
  
  int httpCode = http.POST(requestBody);
  
  if (httpCode == 200) {
    String response = http.getString();
    DynamicJsonDocument responseDoc(1024);
    deserializeJson(responseDoc, response);
    
    if (strcmp(responseDoc["status"], "success") == 0) {
      Serial.println("✅ RFID card registered as pending");
      http.end();
      return true;
    }
  }
  
  http.end();
  return false;
}

// ═══════════════════════════════════════════════════════════════════════════
// BUZZER & LED CONTROL
// ═══════════════════════════════════════════════════════════════════════════

void playBuzzer(BuzzerPattern pattern) {
  if (silentMode && pattern != BUZZER_STARTUP) return;
  
  buzzer.pattern = pattern;
  buzzer.startTime = millis();
  buzzer.active = true;
}

void updateBuzzer() {
  if (!buzzer.active) return;
  
  unsigned long elapsed = millis() - buzzer.startTime;
  
  switch (buzzer.pattern) {
    case BUZZER_SUCCESS:
      if (elapsed < 100) {
        digitalWrite(BUZZER_PIN, HIGH);
      } else if (elapsed < 200) {
        digitalWrite(BUZZER_PIN, LOW);
      } else if (elapsed < 300) {
        digitalWrite(BUZZER_PIN, HIGH);
      } else {
        digitalWrite(BUZZER_PIN, LOW);
        buzzer.active = false;
      }
      break;
      
    case BUZZER_ERROR:
      if (elapsed < 500) {
        digitalWrite(BUZZER_PIN, HIGH);
      } else {
        digitalWrite(BUZZER_PIN, LOW);
        buzzer.active = false;
      }
      break;
      
    case BUZZER_STARTUP:
      if (elapsed < 100) {
        digitalWrite(BUZZER_PIN, HIGH);
      } else if (elapsed < 200) {
        digitalWrite(BUZZER_PIN, LOW);
      } else if (elapsed < 300) {
        digitalWrite(BUZZER_PIN, HIGH);
      } else if (elapsed < 400) {
        digitalWrite(BUZZER_PIN, LOW);
      } else if (elapsed < 500) {
        digitalWrite(BUZZER_PIN, HIGH);
      } else {
        digitalWrite(BUZZER_PIN, LOW);
        buzzer.active = false;
      }
      break;
      
    default:
      buzzer.active = false;
      break;
  }
}

void blinkLED(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(100);
    digitalWrite(LED_PIN, LOW);
    delay(100);
  }
}
