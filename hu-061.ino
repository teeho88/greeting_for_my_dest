/**
 * ESP8266 ESP-01 Clock and Weather Firmware
 *
 * This firmware uses an SSD1306 128x64 OLED display to show alternating screens:
 * - Clock screen: shows day of week, current time (HH:MM), temperature & humidity, and date.
 * - Weather screen: shows city name, current temperature, weather condition, humidity, wind speed, and pressure.
 *
 * Wi-Fi Configuration:
 * If no configuration is found or if a button on GPIO3 is held at boot, the device
 * starts as an access point (SSID: ESP01-Setup) and serves a configuration page.
 * The page allows setting Wi-Fi SSID, password, city for weather, and timezone.
 * Settings are saved in EEPROM and the device reboots to normal mode.
 *
 * Normal Operation:
 * Connects to configured Wi-Fi and retrieves time via NTP (synchronized every 60s).
 * Retrieves weather from wttr.in for the specified city (HTTPS GET request).
 * Displays time and weather data on the OLED, switching screens every 15 seconds.
 *
 * Hardware:
 * - ESP8266 ESP-01 (GPIO0=SDA, GPIO2=SCL for I2C OLED; GPIO3 as input for config button).
 * - OLED display SSD1306 128x64 via I2C (address 0x3C).
 *
 * Note: Uses custom fonts (FreeMonoBold18pt7b for time, FreeMonoBold12pt7b for temperature).
 * All other text uses default font. Display is rotated 180 degrees (setRotation(2)).
 */
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <DNSServer.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeMonoBold18pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <time.h>
#include <Updater.h>

extern "C" {
  #include "user_interface.h"
}

// Pin definitions (ESP-01):
const uint8_t SDA_PIN = 0;           // I2C SDA connected to GPIO0
const uint8_t SCL_PIN = 2;           // I2C SCL connected to GPIO2
const uint8_t BUTTON_PIN = 3;        // Button on GPIO3 (RX pin)

// EEPROM addresses for configuration data:
const int EEPROM_SIZE = 1024;
const int ADDR_SSID = 0;
const int ADDR_PASS = 70;
const int ADDR_CITY = 140;
const int ADDR_TZ   = 210;  // 4 bytes (int32) for timezone offset in seconds
const int ADDR_GREETING = 220;
const int ADDR_FIRMWARE_URL = 430;
const int ADDR_SIGNATURE = 600;  // 4-byte signature "CFG1" to indicate valid config
const int ADDR_ETAG = 610;
const int ADDR_LUCKY_URL = 710;

// Wi-Fi and server:
ESP8266WebServer server(80);
DNSServer dnsServer;
const char *AP_SSID = "Puppy's clock";  // Access Point SSID for config mode
const String firmwareVersion = "v1.1.37";
#define TIME_HEADER_MSG "Happy new year! <3"

// Display:
Adafruit_SSD1306 display(128, 64, &Wire, -1);
bool displayInitialized = false;

// Time and weather:
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);
unsigned long lastScreenSwitch = 0;
int currentScreen = 0;
bool displayReady = false;
unsigned long lastWeatherFetch = 0;
unsigned long lastForecastFetch = 0;
unsigned long lastWifiReconnectAttempt = 0;

// Configuration variables:
String wifiSSID = "";
String wifiPass = "";
String city = "";
String greetingUrl = "";
String firmwareUrl = "";
String luckyImageUrl = "";
int timezoneOffset = 0;  // in seconds

// Weather data variables:
String weatherTemp = "N/A";
String weatherCond = "";
String weatherHum = "";
String weatherWind = "";
String weatherPress = "";
bool weatherValid = false;

// Forecast data
struct Forecast {
  String fullText;
};
Forecast forecasts[3];
bool forecastValid = false;

// Greeting data
String currentGreeting = "Loading...";
unsigned long lastGreetingUpdate = 0;
int greetingIndex = 0;

// Lucky Number data
int luckyNumber = 0;
int lastLuckyDay = -1;
uint8_t* luckyImageBuffer = NULL;
bool luckyImageValid = false;

// System Modes: 0=Normal, 1=Config, 2=Update, 3=SleepConfirm, 4=OTA_GitHub
int systemMode = 0;
bool apServerRunning = false;
bool sleepSelectedYes = false; // false = Không, true = Có
unsigned long lastOTACheck = 0;
String currentFirmwareETag = "";
String pendingOTA_ETag = "";

// Snow effect data
const int NUM_HEARTS = 10;
struct Heart {
  float x;
  float y;
  float speed;
  uint8_t size;
};
Heart hearts[NUM_HEARTS];
bool heartsInitialized = false;

// Weather Task Variables
enum WeatherTaskState {
  W_IDLE,
  W_CONNECTING,
  W_WAIT_HEADER,
  W_READ_BODY
};
WeatherTaskState weatherTaskState = W_IDLE;
WiFiClientSecure weatherClient;
unsigned long weatherTaskTimer = 0;
String weatherResponseBuffer = "";

// Forecast Task Variables
enum ForecastTaskState {
  F_IDLE,
  F_CONNECTING,
  F_WAIT_HEADER,
  F_READ_BODY
};
ForecastTaskState forecastTaskState = F_IDLE;
WiFiClientSecure forecastClient;
unsigned long forecastTaskTimer = 0;
String lastForecastError = "Wait...";

// Function prototypes:
void loadSettings();
void saveSettings();
void startConfigPortal();
void handleConfigForm();
void drawTimeScreen();
void drawWeatherScreen();
void drawForecastScreen();
void drawGreetingScreen();
void startUpdatePortal();
void drawLuckyNumberScreen();
void handleWeatherTask();
void parseWeatherData(String result);
String removeAccents(String str);
void handleForecastTask();
void parseForecastData(WiFiClientSecure& stream);
void updateGreeting();
void updateLuckyImage();
void drawDynamicBackground();
void drawSleepConfirmScreen();
void checkForFirmwareUpdate();
void startOTAUpdate(String targetETag = "");
void enterSleep();

void setup() {

    // Initialize display
    Wire.begin(SDA_PIN, SCL_PIN);
    if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
      displayInitialized = true;
      display.setRotation(2);
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.println(F("Booting..."));
      display.display();
      displayReady = true;
    } else {
      // Leave displayInitialized as false
    }

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  // Initialize EEPROM and load stored settings if available
  EEPROM.begin(EEPROM_SIZE);
  bool configMode = false;

  // Check if config signature is present in EEPROM
  if (EEPROM.read(ADDR_SIGNATURE) == 'C' &&
      EEPROM.read(ADDR_SIGNATURE + 1) == 'F' &&
      EEPROM.read(ADDR_SIGNATURE + 2) == 'G' &&
      EEPROM.read(ADDR_SIGNATURE + 3) == '1') {
  } else {
  }

  // Check button press in first 300ms of boot
  bool buttonPressed = false;
  unsigned long startTime = millis();
  unsigned long lowCount = 0;
  // Tăng thời gian kiểm tra và yêu cầu giữ nút liên tục để tránh nhiễu
  while (millis() - startTime < 500) {
    if (digitalRead(BUTTON_PIN) == LOW) {
      lowCount++;
    } else {
      lowCount = 0;
    }
    delay(10);
    if (lowCount > 10) { // Giữ liên tục > 100ms (10 * 10ms)
      buttonPressed = true;
      break;
    }
  }

  // Determine if we should start config portal
  if (EEPROM.read(ADDR_SIGNATURE) != 'C' || EEPROM.read(ADDR_SIGNATURE+1) != 'F' ||
      EEPROM.read(ADDR_SIGNATURE+2) != 'G' || EEPROM.read(ADDR_SIGNATURE+3) != '1' ||
      buttonPressed) {
    configMode = true;
    systemMode = 1;
  }

  if (configMode) {
    // Load existing settings (if any) to pre-fill form
    if (EEPROM.read(ADDR_SIGNATURE) == 'C' && EEPROM.read(ADDR_SIGNATURE + 1) == 'F' &&
        EEPROM.read(ADDR_SIGNATURE + 2) == 'G' && EEPROM.read(ADDR_SIGNATURE + 3) == '1') {
      loadSettings();
    }
    // Start configuration portal (Access Point mode)
    startConfigPortal();
    // After configuration, ESP will reboot. If not rebooted (e.g. user didn't submit),
    // it will remain in AP mode and handleClient in loop.
  } else {
    // Load settings from EEPROM
    loadSettings();

    if (displayInitialized) {
      display.clearDisplay();
      display.setFont(NULL);
      display.setCursor(0, 0);
      display.print(F("FW Ver: ")); // Already F()
      display.println(firmwareVersion);
      display.println(F("Connecting to WiFi:")); // Already F()
      display.println(wifiSSID);
      display.display();
    }

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
    // Wait up to 10 seconds for connection
    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 10000) {
      delay(500);
      if (displayInitialized) {
        display.print(".");
        display.display();
      }
    }
    if (WiFi.status() == WL_CONNECTED) {
      if (displayInitialized) {
        display.println();
        display.println(F("Connected!")); // Already F()
        display.display();
      }
    } else {
      startConfigPortal();
      return; // Exit setup to avoid running normal mode without WiFi
    }
    // Setup NTP client with stored timezone offset
    timeClient.setPoolServerName("pool.ntp.org");
    timeClient.setTimeOffset(timezoneOffset);
    timeClient.setUpdateInterval(900000);  // 15 mins interval
    timeClient.begin();

    if (displayInitialized) {
      display.println(F("Syncing Time...")); // Already F()
      display.display();
    }
    // Perform initial NTP update
    timeClient.update();

    // Prepare first weather fetch
    lastWeatherFetch = 0; // force immediate fetch on first weather screen display
    weatherValid = false;
  }

  if (displayInitialized) {
    display.println(F("Fetching Data...")); // Already F()
    display.display();
  }

  // Initial greeting fetch
  updateGreeting();
  lastGreetingUpdate = millis();
  
  if (luckyImageUrl != "") {
    updateLuckyImage();
  }
  // Schedule first OTA check 30 seconds after boot (instead of 10 mins)
  lastOTACheck = millis() - 600000UL + 30000UL;

  weatherTaskState = W_IDLE;
  forecastTaskState = F_IDLE;
}

void loop() {
  // Button handling for mode switching
  static unsigned long btnPressStart = 0;
  static bool btnActionTaken = false;

  if (digitalRead(BUTTON_PIN) == LOW) {
    if (btnPressStart == 0) {
      btnPressStart = millis();
      btnActionTaken = false;
    }
    // Normal Mode: Hold > 2s to switch to Config Mode
    if (millis() - btnPressStart > 2000 && !btnActionTaken) {
      if (systemMode == 0) {
        // Normal -> Sleep Confirm
        systemMode = 3;
        sleepSelectedYes = false; // Default "Không"
        drawSleepConfirmScreen();
        btnActionTaken = true;
      } else if (systemMode == 3) {
        // Sleep Confirm -> Action
        if (sleepSelectedYes) {
           // Chọn "Có" -> Tắt (Deep Sleep)
           enterSleep();
        } else {
           // Chọn "Không" -> Config Mode
           systemMode = 1;
           startConfigPortal();
        }
        btnActionTaken = true;
      } else if (systemMode == 1) {
        // Config -> Update
        systemMode = 2;
        startUpdatePortal();
        btnActionTaken = true;
      } else if (systemMode == 2) {
        // Update Manual -> OTA GitHub
        systemMode = 4;
        startOTAUpdate();
        btnActionTaken = true;
      }
    }
  } else {
    if (btnPressStart != 0) {
      // Button released
      // Debounce: Chỉ nhận click nếu thời gian nhấn > 50ms
      if (millis() - btnPressStart > 50) {
        if (!btnActionTaken && systemMode == 3) {
          // Sleep Confirm Mode: Click to toggle selection
          sleepSelectedYes = !sleepSelectedYes;
          drawSleepConfirmScreen();
        } else if (!btnActionTaken && systemMode == 1) {
          // Config Mode: Click to return to Normal Mode (Reboot)
          display.clearDisplay();
          display.setCursor(0, 0);
          display.println(F("Exit Config Mode"));
          display.display();
          delay(1000);
          ESP.restart();
        } else if (!btnActionTaken && systemMode == 0) {
          // Normal Mode Click: Switch to Lucky Number
          currentScreen = 4;
          lastScreenSwitch = millis();
        } else if (!btnActionTaken && systemMode == 2) {
          // Update Mode Click: Exit/Reboot
          display.clearDisplay();
          display.setCursor(0, 0);
          display.println(F("Exit Update Mode"));
          display.display();
          delay(1000);
          ESP.restart();
        }
      }
      btnPressStart = 0;
      btnActionTaken = false;
    }
  }

  // If in config portal mode, handle web server
  if (systemMode == 1 || systemMode == 2) {
    dnsServer.processNextRequest();
    server.handleClient();
    // In AP mode, do not run normal display loop
    return;
  }
  
  if (systemMode == 3) {
    return; // Wait for user interaction in Sleep Confirm mode
  }

  if (systemMode == 4) {
    return;
  }

  // Regular mode: update time and handle display
  timeClient.update();
  unsigned long now = millis();

  // Auto-reconnect WiFi if lost (check every 60s)
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWifiReconnectAttempt > 60000UL) {
      lastWifiReconnectAttempt = now;
      if (displayInitialized) {
        display.clearDisplay();
        display.setFont(NULL);
        display.setCursor(0, 0);
        display.println(F("Lost WiFi!"));
        display.println(F("Reconnecting..."));
        display.display();
      }
      WiFi.reconnect();
      delay(2000);
    }
  }

  // Lucky Number Update Logic (at 00:00 or first run)
  time_t epoch = timeClient.getEpochTime();
  struct tm *ptm = gmtime(&epoch);
  if (ptm->tm_mday != lastLuckyDay) {
    randomSeed(micros());
    luckyNumber = random(0, 100); // 0 to 99
    if (luckyImageUrl != "") {
      updateLuckyImage();
    }
    lastLuckyDay = ptm->tm_mday;
  }

  // Update greeting every 5 minutes
  if (now - lastGreetingUpdate > 300000UL) {
    updateGreeting();
    lastGreetingUpdate = now;
  }
  
  // Check firmware update every 10 minute
  if (now - lastOTACheck > 600000UL) {
    checkForFirmwareUpdate();
    lastOTACheck = now;
  }
  
  // Handle pending OTA update in the main loop to ensure clean stack/heap
  if (pendingOTA_ETag != "") {
     startOTAUpdate(pendingOTA_ETag);
     pendingOTA_ETag = "";
  }

  // Switch screen logic
  unsigned long interval = 15000;
  if (currentScreen == 2) interval = 45000; // Longer duration for forecast screen
  if (currentScreen == 3) interval = 45000; // Greeting screen duration
  if (currentScreen == 4) interval = 10000; // Duration for lucky number

  if (now - lastScreenSwitch > interval) {
    if (currentScreen == 4) {
      currentScreen = 0; // Return to Time screen
    } else {
      currentScreen = (currentScreen + 1) % 4;
    }
    lastScreenSwitch = now;
    
    if (currentScreen == 2) { // Forecast
      if (millis() - lastForecastFetch > 3600000UL || !forecastValid) { // Every 1 hour
        if (forecastTaskState == F_IDLE && weatherTaskState == W_IDLE) {
          forecastTaskState = F_CONNECTING;
        }
      }
    }
  }

  // Run Weather Task
  handleWeatherTask();
  handleForecastTask();

  // Draw the appropriate screen
  switch (currentScreen) {
    case 0: drawTimeScreen(); break;
    case 1: drawWeatherScreen(); break;
    case 2: drawForecastScreen(); break;
    case 3: drawGreetingScreen(); break;
    case 4: drawLuckyNumberScreen(); break;
  }

  // Small delay to yield to system
  delay(10);
}

void startConfigPortal() {
  // Stop any existing WiFi and start AP
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);
  IPAddress apIP = WiFi.softAPIP();

  // Start DNS Server for Captive Portal (redirect all to AP IP)
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", apIP);

    if (displayReady && systemMode == 1) {
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.setFont(NULL);
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.println(F("Config mode"));
      display.setCursor(0, 10);
      display.println(F("SSID: ") + String(AP_SSID));
      display.setCursor(0, 20);
      display.println(F("IP: 192.168.4.1"));
      display.display();
    }

  // Setup web server routes
  if (!apServerRunning) {
    server.on("/", HTTP_GET, []() {
    if (systemMode == 1) {
    // HTML page for config
    String page;
    page.reserve(2048);
    page = F("<!DOCTYPE html><html><head><meta charset='UTF-8'>");
    page += F("<title>ESP8266 Setup</title><style>");
    page += F(".container{max-width:300px;margin:40px auto;padding:20px;background:#f7f7f7;border:1px solid #ccc;border-radius:5px;}");
    page += F("body{text-align:center;font-family:sans-serif;}h2{margin-bottom:15px;}label{display:block;text-align:left;margin-top:10px;}");
    page += F("input, select{width:100%;padding:8px;margin-top:5px;border:1px solid #ccc;border-radius:3px;}");
    page += F("input[type=submit]{margin-top:15px;background:#4caf50;color:white;border:none;cursor:pointer;border-radius:3px;font-size:16px;}");
    page += F("input[type=submit]:hover{background:#45a049;}");
    page += F("</style></head><body><div class='container'>");
    page += F("<h2>Device Configuration</h2><form method='POST' action='/'>");
    // WiFi SSID field
    page += F("<label>Wi-Fi SSID:</label><input type='text' name='ssid' value='") + wifiSSID + F("' required>");
    // Password field
    page += F("<label>Password:</label><input type='password' name='pass' value='") + wifiPass + F("' placeholder=''>");
    // City field
    page += F("<label>City:</label><input type='text' name='city' value='") + city + F("' required>");
    page += F("<label>Greeting URL (Gist Raw):</label><input type='text' name='greeting' value='") + greetingUrl + F("'>");
    page += F("<label>Firmware URL (.bin):</label><input type='text' name='firmware' value='") + firmwareUrl + F("'>");
    page += F("<label>Lucky Image URL (1KB Bin):</label><input type='text' name='lucky_img' value='") + luckyImageUrl + F("'>");
    // Timezone dropdown
    page += F("<label>Timezone:</label><select name='tz'>");
    // Populate timezone options from UTC-12 to UTC+14
    for (int tzHour = -12; tzHour <= 14; ++tzHour) {
      long tzSeconds = tzHour * 3600;
      String option = F("<option value='");
      option += String(tzSeconds) + "'";
      if (tzSeconds == timezoneOffset) {
        option += F(" selected");
      }
      option += F(">UTC");
      if (tzHour >= 0) option += F("+") + String(tzHour);
      else option += String(tzHour);
      option += F("</option>");
      page += option;
    }
    page += F("</select>");
    // Submit button
    page += F("<input type='submit' value='Save'></form></div></body></html>");
    server.send(200, "text/html", page);
    } else if (systemMode == 2) {
      // HTML page for Update
      String page = F("<!DOCTYPE html><html><body><h2>Firmware Update</h2>");
      page += F("<form method='POST' action='/update' enctype='multipart/form-data'>");
      page += F("<input type='file' name='update'><br><br>");
      page += F("<input type='submit' value='Update'></form></body></html>");
      server.send(200, "text/html", page);
    }
    });

    server.on("/", HTTP_POST, handleConfigForm);
    
    // Update Firmware Handler
    server.on("/update", HTTP_POST, []() {
      server.send(200, "text/plain", (Update.hasError()) ? "Update Failed" : "Update Success! Rebooting...");
      ESP.restart();
    }, []() {
      HTTPUpload& upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) {
        WiFiUDP::stopAll();
        uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        if (!Update.begin(maxSketchSpace)) {
          // Error
        }
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
          // Error
        }
      } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
          // Success
        }
      }
      yield();
    });

    // Redirect all other requests to root (Captive Portal requirement)
    server.onNotFound([]() {
      server.sendHeader("Location", String("http://192.168.4.1/"), true);
      server.send(302, "text/plain", "");
    });

    server.begin();
    apServerRunning = true;
  }
}

void startUpdatePortal() {
  display.clearDisplay();
  display.setFont(NULL);
  display.setCursor(0, 0);
  display.println(F("Update Mode"));
  display.println(F("IP: 192.168.4.1"));
  display.println(F("Upload .bin file"));
  display.display();
}

void handleConfigForm() {
  // Get form values
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  String newCity = server.arg("city");
  String newGreeting = server.arg("greeting");
  String newFirmware = server.arg("firmware");
  String newLuckyImg = server.arg("lucky_img");
  String tzStr = server.arg("tz");

  if (ssid.length() > 0 && newCity.length() > 0 && tzStr.length() > 0) {
    wifiSSID = ssid;
    wifiPass = pass;
    city = newCity;
    greetingUrl = newGreeting;
    firmwareUrl = newFirmware;
    luckyImageUrl = newLuckyImg;
    if (luckyImageUrl == "" && luckyImageBuffer) {
       free(luckyImageBuffer);
       luckyImageBuffer = NULL;
       luckyImageValid = false;
    }
    timezoneOffset = tzStr.toInt();
    // Save to EEPROM
    saveSettings();
    // Send response page
    server.send(200, "text/html", F("<html><body><h3>Settings saved. Rebooting...</h3></body></html>"));
    delay(1000);
    ESP.restart();
  } else {
    server.send(400, "text/html", F("<html><body><h3>Invalid input, please fill all required fields.</h3></body></html>"));
  }
}

void loadSettings() {
  // Read SSID
  uint8_t len = EEPROM.read(ADDR_SSID);
  if (len > 0 && len < 0xFF) {
    char buf[80];
    for (int i = 0; i < len; ++i) {
      buf[i] = char(EEPROM.read(ADDR_SSID + 1 + i));
    }
    buf[len] = '\0';
    wifiSSID = String(buf);
  } else {
    wifiSSID = "";
  }
  // Read Password
  len = EEPROM.read(ADDR_PASS);
  if (len > 0 && len < 0xFF) {
    char buf[80];
    for (int i = 0; i < len; ++i) {
      buf[i] = char(EEPROM.read(ADDR_PASS + 1 + i));
    }
    buf[len] = '\0';
    wifiPass = String(buf);
  } else {
    wifiPass = "";
  }
  // Read City
  len = EEPROM.read(ADDR_CITY);
  if (len > 0 && len < 0xFF) {
    char buf[80];
    for (int i = 0; i < len; ++i) {
      buf[i] = char(EEPROM.read(ADDR_CITY + 1 + i));
    }
    buf[len] = '\0';
    city = String(buf);
  } else {
    city = "";
  }
  // Read Greeting URL
  len = EEPROM.read(ADDR_GREETING);
  if (len > 0 && len < 0xFF) {
    char buf[150];
    for (int i = 0; i < len && i < 149; ++i) {
      buf[i] = char(EEPROM.read(ADDR_GREETING + 1 + i));
    }
    buf[len] = '\0';
    greetingUrl = String(buf);
  } else {
    greetingUrl = "";
  }
  // Read Firmware URL
  len = EEPROM.read(ADDR_FIRMWARE_URL);
  if (len > 0 && len < 0xFF) {
    char buf[150];
    for (int i = 0; i < len && i < 149; ++i) {
      buf[i] = char(EEPROM.read(ADDR_FIRMWARE_URL + 1 + i));
    }
    buf[len] = '\0';
    firmwareUrl = String(buf);
  } else {
    firmwareUrl = "";
  }
  // Read ETag
  len = EEPROM.read(ADDR_ETAG);
  if (len > 0 && len < 0xFF) {
    char buf[100];
    for (int i = 0; i < len && i < 99; ++i) {
      buf[i] = char(EEPROM.read(ADDR_ETAG + 1 + i));
    }
    buf[len] = '\0';
    currentFirmwareETag = String(buf);
    currentFirmwareETag.trim();
  } else {
    currentFirmwareETag = "";
  }
  // Read Lucky Image URL
  len = EEPROM.read(ADDR_LUCKY_URL);
  if (len > 0 && len < 0xFF) {
    char buf[200];
    for (int i = 0; i < len && i < 199; ++i) {
      buf[i] = char(EEPROM.read(ADDR_LUCKY_URL + 1 + i));
    }
    buf[len] = '\0';
    luckyImageUrl = String(buf);
  } else {
    luckyImageUrl = "";
  }
  // Read Timezone offset (int32)
  uint32_t b0 = EEPROM.read(ADDR_TZ);
  uint32_t b1 = EEPROM.read(ADDR_TZ + 1);
  uint32_t b2 = EEPROM.read(ADDR_TZ + 2);
  uint32_t b3 = EEPROM.read(ADDR_TZ + 3);
  uint32_t raw = (b0 & 0xFF) | ((b1 & 0xFF) << 8) | ((b2 & 0xFF) << 16) | ((b3 & 0xFF) << 24);
  timezoneOffset = (int) raw;
}

void saveSettings() {
  // Write SSID
  uint8_t len = wifiSSID.length();
  if (len > 0) {
    EEPROM.write(ADDR_SSID, len);
    for (int i = 0; i < len; ++i) {
      EEPROM.write(ADDR_SSID + 1 + i, wifiSSID[i]);
    }
  } else {
    EEPROM.write(ADDR_SSID, 0);
  }
  // Write Password
  len = wifiPass.length();
  if (len > 0) {
    EEPROM.write(ADDR_PASS, len);
    for (int i = 0; i < len; ++i) {
      EEPROM.write(ADDR_PASS + 1 + i, wifiPass[i]);
    }
  } else {
    EEPROM.write(ADDR_PASS, 0);
  }
  // Write City
  len = city.length();
  if (len > 0) {
    EEPROM.write(ADDR_CITY, len);
    for (int i = 0; i < len; ++i) {
      EEPROM.write(ADDR_CITY + 1 + i, city[i]);
    }
  } else {
    EEPROM.write(ADDR_CITY, 0);
  }
  // Write Greeting URL
  len = greetingUrl.length();
  if (len > 200) len = 200; // Limit length
  if (len > 0) {
    EEPROM.write(ADDR_GREETING, len);
    for (int i = 0; i < len; ++i) {
      EEPROM.write(ADDR_GREETING + 1 + i, greetingUrl[i]);
    }
  } else {
    EEPROM.write(ADDR_GREETING, 0);
  }
  // Write Firmware URL
  len = firmwareUrl.length();
  if (len > 150) len = 150;
  if (len > 0) {
    EEPROM.write(ADDR_FIRMWARE_URL, len);
    for (int i = 0; i < len; ++i) {
      EEPROM.write(ADDR_FIRMWARE_URL + 1 + i, firmwareUrl[i]);
    }
  } else {
    EEPROM.write(ADDR_FIRMWARE_URL, 0);
  }
  // Write Lucky Image URL
  len = luckyImageUrl.length();
  if (len > 200) len = 200;
  if (len > 0) {
    EEPROM.write(ADDR_LUCKY_URL, len);
    for (int i = 0; i < len; ++i) {
      EEPROM.write(ADDR_LUCKY_URL + 1 + i, luckyImageUrl[i]);
    }
  } else {
    EEPROM.write(ADDR_LUCKY_URL, 0);
  }
  // Write Timezone (int32)
  int tz = timezoneOffset;
  EEPROM.write(ADDR_TZ, tz & 0xFF);
  EEPROM.write(ADDR_TZ + 1, (tz >> 8) & 0xFF);
  EEPROM.write(ADDR_TZ + 2, (tz >> 16) & 0xFF);
  EEPROM.write(ADDR_TZ + 3, (tz >> 24) & 0xFF);
  // Write signature 'CFG1'
  EEPROM.write(ADDR_SIGNATURE, 'C');
  EEPROM.write(ADDR_SIGNATURE + 1, 'F');
  EEPROM.write(ADDR_SIGNATURE + 2, 'G');
  EEPROM.write(ADDR_SIGNATURE + 3, '1');
  // Commit changes to EEPROM
  EEPROM.commit();
}

void drawSleepConfirmScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setFont(NULL);
  display.setTextSize(1);
  
  // Question
  display.setCursor(0, 0);
  display.println(F("Cun co muon tat"));
  display.println(F("dong ho?"));
  
  // Options
  int yPos = 40;
  
  // Option "Có"
  display.setCursor(20, yPos);
  if (sleepSelectedYes) display.print(F("> "));
  display.print(F("Co"));
  
  // Option "Không"
  display.setCursor(80, yPos);
  if (!sleepSelectedYes) display.print(F("> "));
  display.print(F("Khong"));

  display.display();
}

void drawDynamicBackground() {
  // Draw Flower Background (flying up)
  if (!heartsInitialized) {
    for (int i = 0; i < NUM_HEARTS; i++) {
      hearts[i].x = random(0, 128);
      hearts[i].y = random(0, 64);
      hearts[i].speed = random(2, 15) / 10.0;
      hearts[i].size = random(1, 5);
    }
    heartsInitialized = true;
  }

  for (int i = 0; i < NUM_HEARTS; i++) {
    int x = (int)hearts[i].x;
    int y = (int)hearts[i].y;
    uint8_t s = hearts[i].size;

    if (s == 1) {
      // Tiny flower (3x3)
      display.drawPixel(x + 1, y + 1, SSD1306_WHITE); // Center
      display.drawPixel(x + 1, y, SSD1306_WHITE);     // Top
      display.drawPixel(x + 1, y + 2, SSD1306_WHITE); // Bottom
      display.drawPixel(x, y + 1, SSD1306_WHITE);     // Left
      display.drawPixel(x + 2, y + 1, SSD1306_WHITE); // Right
    } else if (s == 2) {
      // Small flower (5x5)
      display.drawPixel(x + 2, y + 2, SSD1306_WHITE); // Center
      display.drawPixel(x, y, SSD1306_WHITE);
      display.drawPixel(x + 4, y, SSD1306_WHITE);
      display.drawPixel(x, y + 4, SSD1306_WHITE);
      display.drawPixel(x + 4, y + 4, SSD1306_WHITE);
    } else if (s == 3) {
      // Medium flower
      display.fillCircle(x + 2, y + 2, 1, SSD1306_WHITE); // Center
      display.drawPixel(x + 2, y, SSD1306_WHITE); // Top
      display.drawPixel(x + 2, y + 4, SSD1306_WHITE); // Bottom
      display.drawPixel(x, y + 2, SSD1306_WHITE); // Left
      display.drawPixel(x + 4, y + 2, SSD1306_WHITE); // Right
    } else {
      // Large flower
      display.drawPixel(x + 3, y + 3, SSD1306_WHITE); // Center
      display.drawCircle(x + 3, y + 1, 1, SSD1306_WHITE); // Top petal
      display.drawCircle(x + 3, y + 5, 1, SSD1306_WHITE); // Bottom petal
      display.drawCircle(x + 1, y + 3, 1, SSD1306_WHITE); // Left petal
      display.drawCircle(x + 5, y + 3, 1, SSD1306_WHITE); // Right petal
    }

    hearts[i].y -= hearts[i].speed;
    if (hearts[i].y < -10) {
      hearts[i].y = 64 + random(0, 20);
      hearts[i].x = random(0, 128);
      hearts[i].speed = random(2, 15) / 10.0;
      hearts[i].size = random(1, 5);
    }
  }
}

void drawTimeScreen() {
  if (!displayInitialized) {
    return;
  }
  
  static int scrollX = 128;
  static unsigned long lastScroll = 0;
  static int currentScreenCheck = -1;

  // Reset scroll when entering screen
  if (currentScreenCheck != currentScreen) {
    scrollX = 128;
    currentScreenCheck = currentScreen;
  }

  // Update scroll position (Speed for Time Screen)
  if (millis() - lastScroll > 30) {
    scrollX -= 1;
    lastScroll = millis();
  }

  display.setTextWrap(false);
  display.clearDisplay();
  
  drawDynamicBackground();

  display.setTextColor(SSD1306_WHITE);
  // Day name at top center
  // Get current epoch time and derive day of week
  time_t epoch = timeClient.getEpochTime();
  // Calculate day of week (0=Sunday .. 6=Saturday)
  struct tm *tm = gmtime(&epoch);
  int wday = tm->tm_wday;  // tm_wday: days since Sunday (0-6)
  String dayName = "";
  switch(wday) {
    case 0: dayName = F("Sunday"); break;
    case 1: dayName = F("Monday"); break;
    case 2: dayName = F("Tuesday"); break;
    case 3: dayName = F("Wednesday"); break;
    case 4: dayName = F("Thursday"); break;
    case 5: dayName = F("Friday"); break;
    case 6: dayName = F("Saturday"); break;
  }
  display.setFont(NULL); // default font
  int16_t x1, y1;
  uint16_t w, h;
  
  String headerText = dayName + " - " + String(TIME_HEADER_MSG);
  display.getTextBounds(headerText, 0, 0, &x1, &y1, &w, &h);
  
  display.setCursor(scrollX, 0);
  display.print(headerText);
  
  if (scrollX < -((int)w)) {
    scrollX = 128;
  }

  // Time HH:MM in large font, centered
  display.setFont(&FreeMonoBold18pt7b);
  // Format time as HH:MM
  char timeBuf[6];
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", timeClient.getHours(), timeClient.getMinutes());
  String timeStr = String(timeBuf);
  display.getTextBounds(timeStr, 0, 30, &x1, &y1, &w, &h);
  int timeX = (128 - w) / 2;
  // Vertically center the text around mid (y=32)
  int timeY = 32 + (h / 2);
  display.setCursor(timeX, timeY);
  display.print(timeStr);

  // Bottom left: temperature and humidity
  display.setFont(NULL);
  display.setCursor(0, 56);
  if (weatherValid && weatherTemp != "N/A" && weatherHum != "") {
    String tempDisplay = weatherTemp;
    tempDisplay.trim();
    display.print(tempDisplay);
    display.print((char)247); // degree symbol
    display.print(F("C "));
    display.print(weatherHum);
  } else {
    display.print(F("N/A"));
  }

  // Bottom right: date dd.mm.yyyy
  tm = gmtime(&epoch);
  int day = tm->tm_mday;
  int month = tm->tm_mon + 1;
  int year = tm->tm_year + 1900;
  char dateBuf[12];
  snprintf(dateBuf, sizeof(dateBuf), "%02d.%02d.%04d", day, month, year);
  String dateStr = String(dateBuf);
  display.getTextBounds(dateStr, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(128 - w, 56);
  display.print(dateStr);

  display.display();
}

void drawWeatherScreen() {
  if (!displayInitialized) {
    return;
  }
  display.setTextWrap(true);
  display.clearDisplay();
  drawDynamicBackground();
  display.setTextColor(SSD1306_WHITE);
  display.setFont(NULL);
  // Top center: city name
  String cityName = city;
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(cityName, 0, 0, &x1, &y1, &w, &h);
  int cityX = (128 - w) / 2;
  display.setCursor(cityX, 0);
  display.print(cityName);
  // Middle center: temperature (big font FreeMonoBold12pt7b)
  display.setFont(&FreeMonoBold12pt7b);
  if (weatherValid && weatherTemp != "N/A") {
    // weatherTemp is e.g. "+12" or "-3" as string (cleaned)
    String tempNum = weatherTemp;
    // Center the numeric part
    display.getTextBounds(tempNum, 0, 30, &x1, &y1, &w, &h);
    int tempX = (128 - (w + 12)) / 2; // leave space for degree and C (~12px)
    int tempY = 30; // baseline for 12pt font around mid screen
    display.setCursor(tempX, tempY);
    display.print(tempNum);
    // Now draw degree symbol and 'C' in default font after the number
    //display.setFont(NULL);
    // Place small degree symbol near top of big text and 'C' after it
    int degX = tempX + w + 3; // position degree just right of number
    int degY = tempY - 16;    // raise small text (approx half big font height)
    if (degY < 0) degY = 0;
    display.setCursor(degX, degY);
    //display.print((char)247);
    display.drawCircle(degX, degY, 2, SSD1306_WHITE);
    display.setCursor(degX + 3, tempY);
    display.setFont(&FreeMonoBold12pt7b);    
    display.print(F("C"));
    display.setFont(NULL);
  } else {
    // If weather not available, show N/A in big font
    String na = F("N/A");
    display.getTextBounds(na, 0, 30, &x1, &y1, &w, &h);
    int naX = (128 - w) / 2;
    display.setCursor(naX, 30);
    display.print(na);
    // No degree symbol or 'C' in this case
  }
  // Below temperature: condition string (centered)
  display.setFont(NULL);
  String cond = weatherValid ? weatherCond : "";
  cond.trim();
  display.getTextBounds(cond, 0, 40, &x1, &y1, &w, &h);
  int condX = (128 - w) / 2;
  display.setCursor(condX, 40);
  display.print(cond);
  // Bottom right: H:% W:m/s P:mm
  display.setFont(NULL);
  if (weatherValid && weatherTemp != "N/A") {
    String bottomStr = String(F("H:")) + weatherHum;
    bottomStr += F(" W:") + weatherWind;
    if (weatherWind != "N/A") bottomStr += F("m/s");
    bottomStr += F(" P:") + weatherPress + F("mm");
    display.getTextBounds(bottomStr, 0, 56, &x1, &y1, &w, &h);
    display.setCursor(128 - w, 56);
    display.print(bottomStr);
  } else {
    String bottomStr = F("H:N/A W:N/A P:N/A");
    display.getTextBounds(bottomStr, 0, 56, &x1, &y1, &w, &h);
    display.setCursor(128 - w, 56);
    display.print(bottomStr);
  }
  display.display();
}

void drawForecastScreen() {
  if (!displayInitialized) return;
  
  static int scrollX = 128;
  static unsigned long lastScroll = 0;
  static int currentScreenCheck = -1;

  // Reset scroll when entering screen
  if (currentScreenCheck != currentScreen) {
    scrollX = 128;
    currentScreenCheck = currentScreen;
  }

  // Update scroll position
  if (millis() - lastScroll > 40) {
    scrollX -= 2;
    lastScroll = millis();
  }

  display.setTextWrap(false);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setFont(NULL);
  
  display.setCursor(0, 0);
  display.println(F("3-Day Forecast"));
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

  if (forecastValid) {
    int maxW = 0;
    for (int i = 0; i < 3; i++) {
      int16_t x1, y1; uint16_t w, h;
      display.getTextBounds(forecasts[i].fullText, 0, 0, &x1, &y1, &w, &h);
      if (w > maxW) maxW = w;
      
      int y = 15 + (i * 16);
      display.setCursor(scrollX, y);
      display.print(forecasts[i].fullText);
    }
    
    // Reset scroll if all text has scrolled off
    if (scrollX < -maxW) {
      scrollX = 128;
    }
  } else {
    display.setCursor(0, 20);
    display.println(F("Loading..."));
    display.println(lastForecastError);
  }
  display.display();
}

void drawLuckyNumberScreen() {
  if (!displayInitialized) return;

  static int scrollX = 128;
  static unsigned long lastScroll = 0;
  static int currentScreenCheck = -1;

  // Reset scroll when entering screen
  if (currentScreenCheck != currentScreen) {
    scrollX = 128;
    currentScreenCheck = currentScreen;
  }

  // Update scroll position
  if (millis() - lastScroll > 30) {
    scrollX -= 2;
    lastScroll = millis();
  }

  display.setTextWrap(false);
  display.clearDisplay();

  // Draw Image if available
  if (luckyImageValid && luckyImageBuffer != NULL) {
      display.drawBitmap(0, 0, luckyImageBuffer, 128, 64, SSD1306_WHITE);
  }
  
  // Title
  display.setTextColor(SSD1306_WHITE, SSD1306_BLACK); // Use black background for text readability
  display.setFont(NULL);
  String title = F("So may man cua em yeu ngay hom nay la \x03\x03\x03 ") + String(luckyNumber);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(scrollX, 0);
  display.print(title);
  
  if (scrollX < -((int)w)) {
    scrollX = 128;
  }

  // Only draw big number if no image is present
  if (!luckyImageValid || luckyImageBuffer == NULL) {
      display.setTextColor(SSD1306_WHITE);
      display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

      // Number
      display.setFont(&FreeMonoBold18pt7b);
      String numStr = String(luckyNumber);
      display.getTextBounds(numStr, 0, 40, &x1, &y1, &w, &h);
      int x = (128 - w) / 2;
      display.setCursor(x, 45);
      display.print(numStr);
  }
  
  display.display();
}

void drawGreetingScreen() {
  if (!displayInitialized) return;
  
  static int scrollX = 128;
  static unsigned long lastScroll = 0;
  static int currentScreenCheck = -1;

  // Reset scroll when entering screen
  if (currentScreenCheck != currentScreen) {
    scrollX = 128;
    currentScreenCheck = currentScreen;
  }

  // Update scroll position
  if (millis() - lastScroll > 13) {
    scrollX -= 2;
    lastScroll = millis();
  }

  display.setTextWrap(false);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  // Title
  display.setFont(NULL);
  display.setCursor(0, 0);
  display.println(F("Love notes for you"));
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

  // Content
  display.setFont(&FreeMonoBold12pt7b);
  String text = (greetingUrl == "") ? String(F("No URL Configured")) : currentGreeting;
  
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(text, 0, 40, &x1, &y1, &w, &h);
  
  display.setCursor(scrollX, 45);
  display.print(text);
  
  if (scrollX < -((int)w)) {
    scrollX = 128;
  }
  display.display();
}

void handleWeatherTask() {
  switch (weatherTaskState) {
    case W_IDLE:
      if (((millis() - lastWeatherFetch > 900000UL) || (currentScreen == 1 && !weatherValid)) && forecastTaskState == F_IDLE) {
        weatherTaskState = W_CONNECTING;
      }
      break;

    case W_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        weatherClient.setInsecure();
        weatherClient.setBufferSizes(1024, 512);
        // Note: connect is blocking on ESP8266
        if (weatherClient.connect("wttr.in", 443)) {
           String encodedCity = city;
           encodedCity.replace(" ", "-"); //Use hyphens for spaces
           weatherClient.print("GET /" + encodedCity + "?format=%t|%C|%h|%w|%P HTTP/1.0\r\n" +
                               "Host: wttr.in\r\n" +
                               "User-Agent: ESP8266-Weather-Clock\r\n" +
                               "Connection: close\r\n\r\n");
           weatherTaskState = W_WAIT_HEADER;
           weatherTaskTimer = millis();
           weatherResponseBuffer = "";
           weatherResponseBuffer.reserve(2048); // Reserve memory to reduce fragmentation
        } else {
           weatherTaskState = W_IDLE;
           lastWeatherFetch = millis() - 840000UL; // Retry in 1 min
        }
      }
      break;

    case W_WAIT_HEADER:
      // Wait for data to arrive
      if (weatherClient.available()) {
        weatherTaskState = W_READ_BODY;
        weatherTaskTimer = millis();
      }
      if (millis() - weatherTaskTimer > 10000) { // Timeout
        weatherClient.stop();
        weatherTaskState = W_IDLE;
      }
      break;

    case W_READ_BODY:
      unsigned long startLoop = millis();
      while (weatherClient.available()) {
        char c = (char)weatherClient.read();
        weatherResponseBuffer += c;
        if (millis() - startLoop > 5) break; // Limit blocking time per loop
      }

      if (!weatherClient.connected() && !weatherClient.available()) {
        parseWeatherData(weatherResponseBuffer);
        weatherClient.stop();
        weatherValid = true;
        lastWeatherFetch = millis();
        weatherTaskState = W_IDLE;
      }
      if (millis() - weatherTaskTimer > 10000) {
        weatherClient.stop();
        weatherTaskState = W_IDLE;
      }
      break;
  }
}

void parseWeatherData(String result) {
  // Strip headers (find double newline)
  int bodyPos = result.indexOf("\r\n\r\n");
  if (bodyPos != -1) {
    result = result.substring(bodyPos + 4);
  }
  result.trim();
  if (result.length() == 0) return;

  // Parse fields: temp|cond|hum|wind|press
  int idx1 = result.indexOf('|');
  int idx2 = result.indexOf('|', idx1 + 1);
  int idx3 = result.indexOf('|', idx2 + 1);
  int idx4 = result.indexOf('|', idx3 + 1);
  if (idx1 < 0 || idx2 < 0 || idx3 < 0 || idx4 < 0) return;

  String tempStr   = result.substring(0, idx1);
  String condStr   = result.substring(idx1 + 1, idx2);
  String humStr    = result.substring(idx2 + 1, idx3);
  String windStr   = result.substring(idx3 + 1, idx4);
  String pressStr  = result.substring(idx4 + 1);

  // Clean Temperature: remove degree symbol and 'C'
  tempStr.replace("°C", "");
  tempStr.replace("°", "");
  tempStr.replace("C", "");
  tempStr.trim();
  weatherTemp = tempStr; // keep + or - sign if present

  // Clean Condition:
  condStr.trim();
  weatherCond = condStr;

  // Clean Humidity: ensure '%' present
  humStr.trim();
  if (!humStr.endsWith("%")) {
    weatherHum = humStr + "%";
  } else {
    weatherHum = humStr;
  }

  // Clean Wind: extract numeric part (km/h)
  String windNum = "";
  for (uint i = 0; i < windStr.length(); ++i) {
    char c = windStr.charAt(i);
    if ((c >= '0' && c <= '9') || c == '.') {
      windNum += c;
    } else if (c == ' ' && windNum.length() > 0) {
      break;
    }
  }
  if (windNum.length() == 0) {
    weatherWind = "N/A";
  } else {
    float windKmh = windNum.toFloat();
    float windMs = windKmh / 3.6;
    int windMsRounded = (int)round(windMs);
    weatherWind = String(windMsRounded);
  }

  // Clean Pressure: extract numeric part (hPa)
  String pressNum = "";
  for (uint i = 0; i < pressStr.length(); ++i) {
    char c = pressStr.charAt(i);
    if ((c >= '0' && c <= '9') || c == '.') {
      pressNum += c;
    } else if (!pressNum.isEmpty()) {
      break;
    }
  }
  if (pressNum.length() == 0) {
    weatherPress = "N/A";
  } else {
    float pressHpa = pressNum.toFloat();
    float pressMm = pressHpa * 0.75006;
    int pressRounded = (int) round(pressMm);
    weatherPress = String(pressRounded);
  }
}

String removeAccents(String str) {
  str.replace("á", "a"); str.replace("à", "a"); str.replace("ả", "a"); str.replace("ã", "a"); str.replace("ạ", "a");
  str.replace("ă", "a"); str.replace("ắ", "a"); str.replace("ằ", "a"); str.replace("ẳ", "a"); str.replace("ẵ", "a"); str.replace("ặ", "a");
  str.replace("â", "a"); str.replace("ấ", "a"); str.replace("ầ", "a"); str.replace("ẩ", "a"); str.replace("ẫ", "a"); str.replace("ậ", "a");
  str.replace("đ", "d"); str.replace("Đ", "D");
  str.replace("é", "e"); str.replace("è", "e"); str.replace("ẻ", "e"); str.replace("ẽ", "e"); str.replace("ẹ", "e");
  str.replace("ê", "e"); str.replace("ế", "e"); str.replace("ề", "e"); str.replace("ể", "e"); str.replace("ễ", "e"); str.replace("ệ", "e");
  str.replace("í", "i"); str.replace("ì", "i"); str.replace("ỉ", "i"); str.replace("ĩ", "i"); str.replace("ị", "i");
  str.replace("ó", "o"); str.replace("ò", "o"); str.replace("ỏ", "o"); str.replace("õ", "o"); str.replace("ọ", "o");
  str.replace("ô", "o"); str.replace("ố", "o"); str.replace("ồ", "o"); str.replace("ổ", "o"); str.replace("ỗ", "o"); str.replace("ộ", "o");
  str.replace("ơ", "o"); str.replace("ớ", "o"); str.replace("ờ", "o"); str.replace("ở", "o"); str.replace("ỡ", "o"); str.replace("ợ", "o");
  str.replace("ú", "u"); str.replace("ù", "u"); str.replace("ủ", "u"); str.replace("ũ", "u"); str.replace("ụ", "u");
  str.replace("ư", "u"); str.replace("ứ", "u"); str.replace("ừ", "u"); str.replace("ử", "u"); str.replace("ữ", "u"); str.replace("ự", "u");
  str.replace("ý", "y"); str.replace("ỳ", "y"); str.replace("ỷ", "y"); str.replace("ỹ", "y"); str.replace("ỵ", "y");
  str.replace("Â", "A"); str.replace("Ă", "A"); str.replace("Ô", "O"); str.replace("Ơ", "O"); str.replace("Ư", "U"); str.replace("Ê", "E");
  return str;
}

void handleForecastTask() {
  switch (forecastTaskState) {
    case F_IDLE:
      if ((millis() - lastForecastFetch > 3600000UL || !forecastValid) && (weatherTaskState == W_IDLE)) {
          forecastTaskState = F_CONNECTING;
      }
      break;

    case F_CONNECTING:
      // This state now performs the entire blocking fetch and parse operation.
      {
          if (WiFi.status() != WL_CONNECTED) {
              lastForecastError = "No WiFi";
              lastForecastFetch = millis() - 3600000UL + 300000UL; // Retry in 5 mins
              forecastTaskState = F_IDLE;
              return;
          }

          forecastClient.setInsecure();
          // Use 16KB buffer to ensure SSL handshake success with Cloudflare
          forecastClient.setBufferSizes(16384, 512);

          if (!forecastClient.connect("wttr.in", 443)) {
              lastForecastError = "Connect Fail";
              lastForecastFetch = millis() - 3600000UL + 300000UL; // Retry in 5 mins
              forecastTaskState = F_IDLE;
              return;
          }

          lastForecastError = "Connected...";
          String encodedCity = removeAccents(city);
          encodedCity.trim();
          encodedCity.replace(" ", "-"); // Use hyphens for spaces
          forecastClient.print("GET /" + encodedCity + "?format=j1&lang=en HTTP/1.1\r\n" +
                               "Host: wttr.in\r\n" +
                               "User-Agent: Mozilla/5.0 (compatible; ESP8266; +http://arduino.cc)\r\n" +
                               "Accept: application/json\r\n" +
                               "Accept-Encoding: identity\r\n" + // IMPORTANT: Request uncompressed data
                               "Connection: close\r\n\r\n");

          // Find the end of the headers. This is a short blocking call.
          if (!forecastClient.find("\r\n\r\n")) {
              lastForecastError = "Header Fail";
              forecastClient.stop();
              lastForecastFetch = millis() - 3600000UL + 300000UL; // Retry in 5 mins
              forecastTaskState = F_IDLE;
              return;
          }

          // Now the stream is at the body, parse it directly with ArduinoJson
          parseForecastData(forecastClient);

          forecastClient.stop();
          forecastTaskState = F_IDLE;
      }
      break;
    
    // These states are no longer used
    case F_WAIT_HEADER:
    case F_READ_BODY:
      forecastTaskState = F_IDLE;
      break;
  }
}

void parseForecastData(WiFiClientSecure& stream) {
  // Use a filter to reduce memory usage by ignoring unused fields
  StaticJsonDocument<200> filter;
  filter["weather"][0]["date"] = true;
  filter["weather"][0]["maxtempC"] = true;
  filter["weather"][0]["mintempC"] = true;
  filter["weather"][0]["hourly"][0]["time"] = true;
  filter["weather"][0]["hourly"][0]["weatherDesc"][0]["value"] = true;

  // Allocate JsonDocument on the HEAP to avoid stack overflow.
  // 5120 bytes is sufficient with filter
  DynamicJsonDocument* doc = new DynamicJsonDocument(5120);
  if (!doc) {
    lastForecastError = "Heap Fail: JD"; // Failed to allocate JsonDocument
    lastForecastFetch = millis() - 3600000UL + 300000UL; // Retry in 5 mins
    return;
  }
  
  // Parse JSON object directly from the stream with filter
  DeserializationError error = deserializeJson(*doc, stream, DeserializationOption::Filter(filter));

  if (error) {
    lastForecastError = "JSON Err: ";
    lastForecastError += error.c_str();
    lastForecastFetch = millis() - 3600000UL + 300000UL; // Retry in 5 mins
    delete doc; // Free memory
    return;
  }
  
  JsonArray weather = (*doc)["weather"];
  if (weather.isNull() || weather.size() < 3) {
    lastForecastError = "No Weather Arr";
    lastForecastFetch = millis() - 3600000UL + 300000UL; // Retry in 5 mins
    delete doc; // Free memory
    return;
  }
  
  for (int i = 0; i < 3; i++) {
    JsonObject day_forecast = weather[i];
    if (day_forecast.isNull()) {
        forecasts[i].fullText = "";
        continue;
    }

    // Extract Date
    const char* date_p = day_forecast["date"];
    String date = date_p ? String(date_p) : "";

    // Extract Max Temp (Search from date position)
    const char* maxT_p = day_forecast["maxtempC"];
    String maxT = maxT_p ? String(maxT_p) : "N/A";

    // Extract Min Temp (Search from date position)
    const char* minT_p = day_forecast["mintempC"];
    String minT = minT_p ? String(minT_p) : "N/A";

    // Extract Hourly descriptions
    String morn = "", noon = "", aft = "", eve = "";
    JsonArray hourly = day_forecast["hourly"];
    if (!hourly.isNull()) {
      for(JsonObject h : hourly) {
          const char* timeStr = h["time"];
          if (!timeStr) continue;
          int timeInt = atoi(timeStr);
          
          const char* desc_p = h["weatherDesc"][0]["value"];
          String desc = desc_p ? String(desc_p) : "";

          if (timeInt == 900) morn = desc;
          else if (timeInt == 1200) noon = desc;
          else if (timeInt == 1500) aft = desc;
          else if (timeInt == 2100) eve = desc;
      }
    }

    // Construct full text
    // Format: "MM-DD: min/max C, Sang: ..., Trua: ..., Chieu: ..., Toi: ..."
    String d = date.substring(5);
    String text = d + F(": ") + minT + F("/") + maxT + String((char)247) + F("C");
    text += F(", Morn: ") + removeAccents(morn);
    text += F(", Noon: ") + removeAccents(noon);
    text += F(", Aft: ") + removeAccents(aft);
    text += F(", Eve: ") + removeAccents(eve);
    
    forecasts[i].fullText = text;
  }

  delete doc; // IMPORTANT: Free the memory

  if (forecasts[0].fullText.length() > 0) {
     forecastValid = true;
     lastForecastError = "Success";
     lastForecastFetch = millis();
  } else {
     lastForecastError = "Parse Fail";
     lastForecastFetch = millis() - 3600000UL + 300000UL; // Retry in 5 mins
  }
}

void updateGreeting() {
  if (WiFi.status() != WL_CONNECTED || greetingUrl == "") {
    return;
  }

  // Use heap allocation for client to avoid stack overflow
  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) return;
  client->setInsecure();
  // Optimize buffer for ESP8266 RAM (Default is too large)
  client->setBufferSizes(1024, 1024);
  client->setTimeout(10000);

  HTTPClient http;
  
  // Add cache buster to URL to avoid getting cached response from GitHub CDN
  String url = greetingUrl;
  if (url.indexOf('?') == -1) {
    url += "?t=" + String(millis());
  } else {
    url += "&t=" + String(millis());
  }

  // Tự động xử lý redirect (quan trọng cho Gist Raw)
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("ESP8266-Weather-Clock");
  http.addHeader("Cache-Control", "no-cache");
  http.useHTTP10(true); // Force HTTP 1.0 to avoid Chunked Transfer Encoding

  if (http.begin(*client, url)) {
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      WiFiClient *stream = http.getStreamPtr();
      int currentIndex = 0;
      bool found = false;
      
      while (http.connected() || (stream && stream->available())) {
        if (stream->available()) {
          String line = stream->readStringUntil('\n');
          line.trim();
          if (line.length() > 0) {
            if (currentIndex == greetingIndex) {
              currentGreeting = line;
              found = true;
              break;
            }
            currentIndex++;
          }
        }
        yield();
      }
      
      if (!found) greetingIndex = 0;
      else greetingIndex++;
    }
    http.end();
  }
  delete client;
}

void updateLuckyImage() {
  if (WiFi.status() != WL_CONNECTED || luckyImageUrl == "") return;

  if (!luckyImageBuffer) {
    luckyImageBuffer = (uint8_t*) malloc(1024);
  }
  if (!luckyImageBuffer) return;

  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) return;
  client->setInsecure();
  client->setBufferSizes(4096, 512);
  client->setTimeout(10000);

  HTTPClient http;
  String url = luckyImageUrl;
  if (url.indexOf('?') == -1) url += "?t=" + String(millis());
  else url += "&t=" + String(millis());

  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("ESP8266-Weather-Clock");
  
  if (http.begin(*client, url)) {
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
       int len = http.getSize();
       if (len > 0) {
          WiFiClient *stream = http.getStreamPtr();
          int total = 0;
          unsigned long start = millis();
          while((http.connected() || stream->available()) && total < 1024 && millis() - start < 5000) {
             if(stream->available()) {
                 luckyImageBuffer[total++] = stream->read();
             }
             yield();
          }
          if (total == 1024) luckyImageValid = true;
       }
    }
    http.end();
  }
  delete client;
}

void enterSleep() {
  display.clearDisplay();
  display.display();
  // Tắt điện áp màn hình để tiết kiệm pin tối đa
  display.ssd1306_command(SSD1306_DISPLAYOFF);

  // Chờ người dùng thả nút ra trước khi ngủ (tránh ngủ rồi dậy ngay lập tức)
  while(digitalRead(BUTTON_PIN) == LOW) {
    yield();
  }

  WiFi.mode(WIFI_OFF);
  
  // Cấu hình Light Sleep
  wifi_set_opmode(NULL_MODE);
  wifi_fpm_set_sleep_type(LIGHT_SLEEP_T);
  wifi_fpm_open();
  
  // Đánh thức khi chân GPIO3 (BUTTON_PIN) xuống mức thấp
  gpio_pin_wakeup_enable(GPIO_ID_PIN(BUTTON_PIN), GPIO_PIN_INTR_LOLEVEL);
  
  // Vòng lặp để ngủ liên tục cho đến khi nút được nhấn (vượt qua giới hạn 268s)
  while (digitalRead(BUTTON_PIN) == HIGH) {
    wifi_fpm_do_sleep(0xFFFFFFF);
    delay(10);
  }
  
  // Sau khi thức dậy -> Khởi động lại để vào Normal Mode sạch sẽ
  ESP.restart();
}

void checkForFirmwareUpdate() {
  if (WiFi.status() != WL_CONNECTED || firmwareUrl == "") return;

  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) return;
  client->setInsecure();
  client->setTimeout(10000);
  
  HTTPClient http;
  String url = firmwareUrl;
  // Add cache buster to ensure we get fresh headers
  if (url.indexOf('?') == -1) url += "?t=" + String(millis());
  else url += "&t=" + String(millis());
  
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("ESP8266-Weather-Clock");
  
  String foundETag = "";

  // Use standard GET (no Range) to ensure headers are received correctly
  if (http.begin(*client, url)) {
    const char * headerKeys[] = {"ETag"};
    http.collectHeaders(headerKeys, 1);
    
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
       String newETag = http.header("ETag");
       
       if (newETag != "") {
         // If local ETag is empty (first run) OR different from server, trigger update
         if (currentFirmwareETag == "" || currentFirmwareETag != newETag) {
           display.clearDisplay();
           display.setFont(NULL);
           display.setCursor(0, 0);
           display.println(F("Update Found!"));
           display.print(F("L:")); display.println(currentFirmwareETag.substring(0, 10));
           display.print(F("R:")); display.println(newETag.substring(0, 10));
           display.display();
           delay(3000);
           pendingOTA_ETag = newETag; // Set flag for loop() to handle
         }
       }
    }
    http.end();
  }
  delete client;
}

void startOTAUpdate(String targetETag) {
  display.clearDisplay();
  display.setFont(NULL);
  display.setCursor(0, 0);
  display.println(F("OTA GitHub Mode"));
  display.display();

  // Debug: Print initial Heap
  display.print(F("Heap: ")); display.println(ESP.getFreeHeap());
  display.display();

  // Free up memory to ensure stable update
  if (luckyImageBuffer) {
    free(luckyImageBuffer);
    luckyImageBuffer = NULL;
  }
  for(int i=0; i<3; i++) forecasts[i].fullText = String();
  currentGreeting = String();
  weatherCond = String();
  weatherTemp = String();
  weatherHum = String();
  weatherWind = String();
  weatherPress = String();
  weatherResponseBuffer = String();
  lastForecastError = String();
  
  // Clear config strings not needed for update
  city = String();
  greetingUrl = String();
  luckyImageUrl = String();

  // Đảm bảo các client khác đã dừng để giải phóng bộ nhớ đệm SSL
  weatherClient.stop();
  forecastClient.stop();
  WiFiClient::stopAll(); // Force stop all clients
  delay(1000); // Allow TCP stack to clean up

  if (WiFi.status() != WL_CONNECTED) {
    display.println(F("Connecting WiFi..."));
    display.display();
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
    
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      delay(500);
      display.print(".");
      display.display();
    }
    
    if (WiFi.status() != WL_CONNECTED) {
      display.println(F("\nWiFi Failed!"));
      display.display();
      delay(2000);
      ESP.restart();
      return;
    }
  }
  
  display.println(F("\nDownloading..."));
  display.print(F("URL: ")); display.println(firmwareUrl.substring(0, 10) + "...");
  display.display();
  
  if (firmwareUrl == "") {
     display.println(F("No URL set!"));
     display.display();
     delay(2000);
     ESP.restart();
     return;
  }

  WiFiClientSecure *client = nullptr;
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  String url = firmwareUrl;
  if (url.indexOf('?') == -1) url += "?t=" + String(millis());
  else url += "&t=" + String(millis());
  
  int httpCode = -1;
  // Retry loop: Try up to 3 times with different buffer configs
  for (int attempt = 0; attempt < 3; attempt++) {
      display.println(F("Connecting..."));
      display.display();
      
      if (client) delete client;
      client = new WiFiClientSecure;
      if (!client) {
          display.println(F("OOM!"));
          display.display();
          delay(1000);
          ESP.restart();
          return;
      }
      client->setInsecure();
      // Attempt 0: 16KB (Standard), Attempt 1: 12KB (Low RAM), Attempt 2: 16KB
      if (attempt == 1) client->setBufferSizes(12288, 512);
      else client->setBufferSizes(16384, 512);
      
      client->setTimeout(15000);
      ESP.wdtFeed();
      
      if (http.begin(*client, url)) {
          httpCode = http.GET();
          if (httpCode == HTTP_CODE_OK) break; // Success
          http.end();
      }
      
      display.print(F("Retry ")); display.println(attempt + 1);
      display.display();
      delay(1000);
  }

  if (httpCode != HTTP_CODE_OK) {
    display.print(F("HTTP Err: ")); display.println(httpCode);
    display.display();
    delay(5000);
    delete client;
    ESP.restart();
    return;
  }

  int contentLength = http.getSize();
  // Nếu không biết kích thước (chunked), giả định kích thước tối đa
  size_t updateSize = (contentLength > 0) ? contentLength : ((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000);
  
  if (!Update.begin(updateSize)) {
    display.println(F("Update.begin Fail"));
    display.print(F("Err: ")); display.println(Update.getError());
    display.display();
    delay(5000);
    ESP.restart();
    return;
  }

  WiFiClient * stream = http.getStreamPtr();
  uint8_t buff[1024];
  int received = 0;
  unsigned long lastDraw = 0;
  unsigned long lastDataTime = millis();

  while (http.connected() || stream->available()) {
    size_t size = stream->available();
    if (size) {
      // Đọc tối đa 128 byte mỗi lần
      int c = stream->readBytes(buff, ((size > sizeof(buff)) ? sizeof(buff) : size));
      if (Update.write(buff, c) != c) {
         display.println(F("Write Error"));
         display.display();
         break;
      }
      received += c;
      lastDataTime = millis();

      // Cập nhật màn hình (giới hạn 5fps để không làm chậm mạng)
      if (millis() - lastDraw > 200) {
        lastDraw = millis();
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println(F("OTA Stream:"));
        display.print(F("Rx: ")); display.println(received);
        if (contentLength > 0) {
           int pct = (received * 100) / contentLength;
           display.print(F("Percents: ")); display.print(pct); display.println(F("%"));
        } else {
           display.println(F("Mode: Chunked"));
        }
        display.print(F("Heap: ")); display.println(ESP.getFreeHeap());
        display.display();
      }
    } else {
      // Timeout nếu không có dữ liệu trong 10s
      if (millis() - lastDataTime > 10000) {
        display.clearDisplay();
        display.setCursor(0,0);
        display.println(F("Update Timeout!"));
        display.display();
        delay(3000);
        ESP.restart();
        return;
      }
      delay(1);
    }
    // Nếu đã tải đủ dung lượng (trường hợp không chunked) thì thoát
    if (contentLength > 0 && received >= contentLength) break;
  }

  // Safety Check: Verify file size integrity before committing
  if (contentLength > 0 && received != contentLength) {
      display.clearDisplay();
      display.setCursor(0,0);
      display.println(F("Download Error!"));
      display.println(F("Incomplete File"));
      display.display();
      delay(5000);
      ESP.restart();
      return;
  }

  http.end();
  delete client;

  if (Update.end(contentLength == -1)) {
      if (targetETag != "") {
        int len = targetETag.length();
        if (len > 90) len = 90;
        EEPROM.write(ADDR_ETAG, len);
        for (int i = 0; i < len; ++i) {
          EEPROM.write(ADDR_ETAG + 1 + i, targetETag[i]);
        }
        EEPROM.commit();
      }
      display.clearDisplay();
      display.setCursor(0,0);
      display.println(F("Update Success!"));
      display.display();
      delay(1000);
      ESP.restart();
  } else {
      display.clearDisplay();
      display.setCursor(0,0);
      display.println(F("Update Failed"));
      display.print(F("Err: ")); display.println(Update.getError());
      display.display();
      delay(5000);
      ESP.restart();
  }
}
