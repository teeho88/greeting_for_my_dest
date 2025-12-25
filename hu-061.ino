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
#include <Adafruit_SSD1306.h>
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

// Wi-Fi and server:
ESP8266WebServer server(80);
const char *AP_SSID = "Puppy's clock";  // Access Point SSID for config mode
const String firmwareVersion = "v1.0.0";

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

// Configuration variables:
String wifiSSID = "";
String wifiPass = "";
String city = "";
String greetingUrl = "";
String firmwareUrl = "";
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

// System Modes: 0=Normal, 1=Config, 2=Update, 3=SleepConfirm, 4=OTA_GitHub
int systemMode = 0;
bool apServerRunning = false;
bool sleepSelectedYes = false; // false = Không, true = Có
unsigned long lastOTACheck = 0;
String currentFirmwareETag = "";

// Snow effect data
const int NUM_SNOWFLAKES = 40;
struct Snowflake {
  float x;
  float y;
  float speed;
};
Snowflake snowflakes[NUM_SNOWFLAKES];
bool snowInitialized = false;

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
bool getWeather();
String removeAccents(String str);
bool getForecast();
void updateGreeting();
void drawDynamicBackground();
void drawSleepConfirmScreen();
void checkForFirmwareUpdate();
void startOTAUpdate();
void enterSleep();

void setup() {

    // Initialize display
    Wire.begin(SDA_PIN, SCL_PIN);
    if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
      displayInitialized = true;
      display.setRotation(2);
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.println("Booting...");
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
  while (millis() - startTime < 300) {
    if (digitalRead(BUTTON_PIN) == LOW) {
      buttonPressed = true;
    }
    delay(10);
  }
  if (buttonPressed) {
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
      display.setTextColor(SSD1306_WHITE);
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print(F("FW Ver: "));
      display.println(firmwareVersion);
      display.println(F("Connecting to WiFi:"));
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
        display.println(F("Connected!"));
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
      display.println(F("Syncing Time..."));
      display.display();
    }
    // Perform initial NTP update
    timeClient.update();

    // Prepare first weather fetch
    lastWeatherFetch = 0; // force immediate fetch on first weather screen display
    weatherValid = false;
  }

  if (displayInitialized) {
    display.println(F("Fetching Data..."));
    display.display();
  }

  // Initial greeting fetch
  updateGreeting();
  lastGreetingUpdate = millis();
  lastOTACheck = millis();

weatherValid = getWeather();
lastWeatherFetch = millis();
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
      if (!btnActionTaken && systemMode == 3) {
        // Sleep Confirm Mode: Click to toggle selection
        sleepSelectedYes = !sleepSelectedYes;
        drawSleepConfirmScreen();
      } else if (!btnActionTaken && systemMode == 1) {
        // Config Mode: Click to return to Normal Mode (Reboot)
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("Exit Config Mode");
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
        display.println("Exit Update Mode");
        display.display();
        delay(1000);
        ESP.restart();
      }
      btnPressStart = 0;
      btnActionTaken = false;
    }
  }

  // If in config portal mode, handle web server
  if (systemMode == 1 || systemMode == 2) {
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

  // Lucky Number Update Logic (at 00:00 or first run)
  time_t epoch = timeClient.getEpochTime();
  struct tm *ptm = gmtime(&epoch);
  if (ptm->tm_mday != lastLuckyDay) {
    randomSeed(micros());
    luckyNumber = random(0, 100); // 0 to 99
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
    
    // Data update logic based on screen
    if (currentScreen == 1) { // Weather
      if (millis() - lastWeatherFetch > 900000UL || !weatherValid) {
        weatherValid = getWeather();
        lastWeatherFetch = millis();
      }
    } else if (currentScreen == 2) { // Forecast
      if (millis() - lastForecastFetch > 3600000UL || !forecastValid) { // Every 1 hour
        forecastValid = getForecast();
        lastForecastFetch = millis();
      }
    }
  }

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

    if (displayReady && systemMode == 1) {
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.setFont(NULL);
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.println("AP mode");
      display.setCursor(0, 10);
      display.println("SSID: " + String(AP_SSID));
      display.setCursor(0, 20);
      display.println("IP: 192.168.4.1");
      display.display();
    }

  // Setup web server routes
  if (!apServerRunning) {
    server.on("/", HTTP_GET, []() {
    if (systemMode == 1) {
    // HTML page for config
    String page = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    page += String("<title>ESP8266 Setup</title><style>") +
            ".container{max-width:300px;margin:40px auto;padding:20px;background:#f7f7f7;border:1px solid #ccc;border-radius:5px;}" +
            "body{text-align:center;font-family:sans-serif;}h2{margin-bottom:15px;}label{display:block;text-align:left;margin-top:10px;}" +
            "input, select{width:100%;padding:8px;margin-top:5px;border:1px solid #ccc;border-radius:3px;}" +
            "input[type=submit]{margin-top:15px;background:#4caf50;color:white;border:none;cursor:pointer;border-radius:3px;font-size:16px;}" +
            "input[type=submit]:hover{background:#45a049;}" +
            "</style></head><body><div class='container'>";
    page += "<h2>Device Configuration</h2><form method='POST' action='/'>";
    // WiFi SSID field
    page += "<label>Wi-Fi SSID:</label><input type='text' name='ssid' value='" + wifiSSID + "' required>";
    // Password field
    page += "<label>Password:</label><input type='password' name='pass' value='" + wifiPass + "' placeholder=''>";
    // City field
    page += "<label>City:</label><input type='text' name='city' value='" + city + "' required>";
    page += "<label>Greeting URL (Gist Raw):</label><input type='text' name='greeting' value='" + greetingUrl + "'>";
    page += "<label>Firmware URL (.bin):</label><input type='text' name='firmware' value='" + firmwareUrl + "'>";
    // Timezone dropdown
    page += "<label>Timezone:</label><select name='tz'>";
    // Populate timezone options from UTC-12 to UTC+14
    for (int tzHour = -12; tzHour <= 14; ++tzHour) {
      long tzSeconds = tzHour * 3600;
      String option = "<option value='" + String(tzSeconds) + "'";
      if (tzSeconds == timezoneOffset) {
        option += " selected";
      }
      option += ">UTC";
      if (tzHour >= 0) option += "+" + String(tzHour);
      else option += String(tzHour);
      option += "</option>";
      page += option;
    }
    page += "</select>";
    // Submit button
    page += "<input type='submit' value='Save'></form></div></body></html>";
    server.send(200, "text/html", page);
    } else if (systemMode == 2) {
      // HTML page for Update
      String page = "<!DOCTYPE html><html><body><h2>Firmware Update</h2>";
      page += "<form method='POST' action='/update' enctype='multipart/form-data'>";
      page += "<input type='file' name='update'><br><br>";
      page += "<input type='submit' value='Update'></form></body></html>";
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

    server.begin();
    apServerRunning = true;
  }
}

void startUpdatePortal() {
  display.clearDisplay();
  display.setFont(NULL);
  display.setCursor(0, 0);
  display.println("Update Mode");
  display.println("IP: 192.168.4.1");
  display.println("Upload .bin file");
  display.display();
}

void handleConfigForm() {
  // Get form values
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  String newCity = server.arg("city");
  String newGreeting = server.arg("greeting");
  String newFirmware = server.arg("firmware");
  String tzStr = server.arg("tz");

  if (ssid.length() > 0 && newCity.length() > 0 && tzStr.length() > 0) {
    wifiSSID = ssid;
    wifiPass = pass;
    city = newCity;
    greetingUrl = newGreeting;
    firmwareUrl = newFirmware;
    timezoneOffset = tzStr.toInt();
    // Save to EEPROM
    saveSettings();
    // Send response page
    server.send(200, "text/html", "<html><body><h3>Settings saved. Rebooting...</h3></body></html>");
    delay(1000);
    ESP.restart();
  } else {
    server.send(400, "text/html", "<html><body><h3>Invalid input, please fill all required fields.</h3></body></html>");
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
  display.println("Cun co muon tat");
  display.println("dong ho?");
  
  // Options
  int yPos = 40;
  
  // Option "Có"
  display.setCursor(20, yPos);
  if (sleepSelectedYes) display.print("> ");
  display.print("Co");
  
  // Option "Không"
  display.setCursor(80, yPos);
  if (!sleepSelectedYes) display.print("> ");
  display.print("Khong");

  display.display();
}

void drawDynamicBackground() {
  // Draw Snow Background
  if (!snowInitialized) {
    for (int i = 0; i < NUM_SNOWFLAKES; i++) {
      snowflakes[i].x = random(0, 128);
      snowflakes[i].y = random(0, 64);
      snowflakes[i].speed = random(5, 25) / 10.0;
    }
    snowInitialized = true;
  }

  for (int i = 0; i < NUM_SNOWFLAKES; i++) {
    display.drawPixel((int)snowflakes[i].x, (int)snowflakes[i].y, SSD1306_WHITE);
    snowflakes[i].y += snowflakes[i].speed;
    if (snowflakes[i].y >= 64) {
      snowflakes[i].y = 0;
      snowflakes[i].x = random(0, 128);
      snowflakes[i].speed = random(5, 25) / 10.0;
    }
  }
}

void drawTimeScreen() {
  if (!displayInitialized) {
    return;
  }
  display.setTextWrap(true);
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
    case 0: dayName = "Sunday"; break;
    case 1: dayName = "Monday"; break;
    case 2: dayName = "Tuesday"; break;
    case 3: dayName = "Wednesday"; break;
    case 4: dayName = "Thursday"; break;
    case 5: dayName = "Friday"; break;
    case 6: dayName = "Saturday"; break;
  }
  display.setFont(NULL); // default font
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(dayName, 0, 0, &x1, &y1, &w, &h);
  // center horizontally
  int dayX = (128 - w) / 2;
  display.setCursor(dayX, 0);
  display.print(dayName);

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
    display.print("C ");
    display.print(weatherHum);
  } else {
    display.print("N/A");
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
    display.print("C");
    display.setFont(NULL);
  } else {
    // If weather not available, show N/A in big font
    String na = "N/A";
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
    String bottomStr = String("H:") + weatherHum;
    bottomStr += " W:" + weatherWind;
    if (weatherWind != "N/A") bottomStr += "m/s";
    bottomStr += " P:" + weatherPress + "mm";
    display.getTextBounds(bottomStr, 0, 56, &x1, &y1, &w, &h);
    display.setCursor(128 - w, 56);
    display.print(bottomStr);
  } else {
    String bottomStr = "H:N/A W:N/A P:N/A";
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
  display.println("3-Day Forecast");
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
    display.println("Loading...");
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
  display.setTextColor(SSD1306_WHITE);
  
  // Title
  display.setFont(NULL);
  String title = "So may man cua em yeu ngay hom nay \x03\x03\x03";
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(scrollX, 0);
  display.print(title);
  
  if (scrollX < -((int)w)) {
    scrollX = 128;
  }

  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

  // Number
  display.setFont(&FreeMonoBold18pt7b);
  String numStr = String(luckyNumber);
  display.getTextBounds(numStr, 0, 40, &x1, &y1, &w, &h);
  int x = (128 - w) / 2;
  display.setCursor(x, 45);
  display.print(numStr);
  
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
  if (millis() - lastScroll > 20) {
    scrollX -= 2;
    lastScroll = millis();
  }

  display.setTextWrap(false);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  // Title
  display.setFont(NULL);
  display.setCursor(0, 0);
  display.println("Greeting");
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

  // Content
  display.setFont(&FreeMonoBold12pt7b);
  String text = (greetingUrl == "") ? "No URL Configured" : currentGreeting;
  
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(text, 0, 40, &x1, &y1, &w, &h);
  
  display.setCursor(scrollX, 45);
  display.print(text);
  
  if (scrollX < -((int)w)) {
    scrollX = 128;
  }
  display.display();
}

bool getWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure(); // Skip certificate validation for simplicity
  client.setTimeout(10000); // Increase timeout to 10s
  const char* host = "wttr.in";
  String encodedCity = city;
  encodedCity.replace(" ", "-");
  String url = "/" + encodedCity + "?format=%t|%C|%h|%w|%P";

  if (!client.connect(host, 443)) {
    return false;
  }

  // Send GET request
  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "User-Agent: ESP8266\r\n" +
               "Connection: close\r\n\r\n");

  // Skip headers
  if (!client.find("\r\n\r\n")) return false;

  // Read body
  String result = client.readStringUntil('\n');
  client.stop();
  
  result.trim();

  if (result.length() == 0) {
    return false;
  }

  // Parse fields: temp|cond|hum|wind|press
  int idx1 = result.indexOf('|');
  int idx2 = result.indexOf('|', idx1 + 1);
  int idx3 = result.indexOf('|', idx2 + 1);
  int idx4 = result.indexOf('|', idx3 + 1);
  if (idx1 < 0 || idx2 < 0 || idx3 < 0 || idx4 < 0) return false;

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

  // Validate critical fields
  if (weatherTemp == "" || weatherCond == "") {
    return false;
  }
  return true;
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

bool getForecast() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10000); // Increase timeout to 10s
  const char* host = "wttr.in";
  String encodedCity = city;
  encodedCity.replace(" ", "-");
  // Use JSON format
  String url = "/" + encodedCity + "?format=j1&lang=en";

  if (!client.connect(host, 443)) return false;

  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "User-Agent: ESP8266\r\n" +
               "Connection: close\r\n\r\n");

  // Skip headers
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break;
    if (line == "\r" || line == "") break;
  }

  // Stream parsing to save memory and handle large responses
  if (!client.find("\"weather\":")) {
    client.stop();
    return false;
  }

  for (int i = 0; i < 3; i++) {
    // Extract Date
    if (!client.find("\"date\":")) break;
    if (!client.find("\"")) break;
    String date = client.readStringUntil('\"');

    // Extract Hourly descriptions
    String morn = "", noon = "", aft = "", eve = "";
    if (client.find("\"hourly\":")) {
      for (int h = 0; h < 8; h++) {
        // Find time
        String timeVal = "";
        if (client.find("\"time\":")) {
          if (client.find("\"")) {
            timeVal = client.readStringUntil('\"');
          }
        }
        
        String desc = "";
        // Look for weatherDesc
        if (client.find("\"weatherDesc\":")) {
          if (client.find("\"value\":")) {
            if (client.find("\"")) {
              desc = client.readStringUntil('\"');
            }
          }
        }
        
        if (timeVal == "900") morn = desc;
        else if (timeVal == "1200") noon = desc;
        else if (timeVal == "1500") aft = desc;
        else if (timeVal == "2100") eve = desc;
      }
    }

    // Extract Max Temp
    if (!client.find("\"maxtempC\":")) break;
    if (!client.find("\"")) break;
    String maxT = client.readStringUntil('\"');

    // Extract Min Temp
    if (!client.find("\"mintempC\":")) break;
    if (!client.find("\"")) break;
    String minT = client.readStringUntil('\"');

    // Construct full text
    // Format: "MM-DD: min/max C, Sang: ..., Trua: ..., Chieu: ..., Toi: ..."
    String d = date.substring(5);
    String text = d + ": " + minT + "/" + maxT + String((char)247) + "C";
    text += ", Morn: " + removeAccents(morn);
    text += ", Noon: " + removeAccents(noon);
    text += ", Aft: " + removeAccents(aft);
    text += ", Eve: " + removeAccents(eve);
    
    forecasts[i].fullText = text;
  }
  client.stop();
  return true;
}

void updateGreeting() {
  if (WiFi.status() != WL_CONNECTED || greetingUrl == "") {
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  // Optimize buffer for ESP8266 RAM (Default is too large)
  client.setBufferSizes(1024, 1024);
  client.setTimeout(10000);

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

  if (http.begin(client, url)) {
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      WiFiClient *stream = http.getStreamPtr();
      int currentIndex = 0;
      bool found = false;
      
      while (http.connected() || stream->available()) {
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

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5000);
  
  HTTPClient http;
  String url = firmwareUrl;
  // Add cache buster to ensure we get fresh headers
  if (url.indexOf('?') == -1) url += "?t=" + String(millis());
  else url += "&t=" + String(millis());
  
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("ESP8266-Weather-Clock");
  
  // Use HEAD request to check headers only (save bandwidth)
  if (http.begin(client, url)) {
    const char * headerKeys[] = {"ETag"};
    http.collectHeaders(headerKeys, 1);
    
    int httpCode = http.sendRequest("HEAD");
    if (httpCode == HTTP_CODE_OK) {
       String newETag = http.header("ETag");
       if (newETag != "") {
         if (currentFirmwareETag == "") {
           // First run, just store the ETag
           currentFirmwareETag = newETag;
         } else if (currentFirmwareETag != newETag) {
           // ETag changed, trigger update!
           startOTAUpdate(); 
         }
       }
    }
    http.end();
  }
}

void startOTAUpdate() {
  display.clearDisplay();
  display.setFont(NULL);
  display.setCursor(0, 0);
  display.println("OTA GitHub Mode");
  display.display();

  if (WiFi.status() != WL_CONNECTED) {
    display.println("Connecting WiFi...");
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
      display.println("\nWiFi Failed!");
      display.display();
      delay(2000);
      ESP.restart();
      return;
    }
  }
  
  display.println("\nDownloading...");
  display.display();
  
  if (firmwareUrl == "") {
     display.println("No URL set!");
     display.display();
     delay(2000);
     ESP.restart();
     return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(1024, 1024);
  
  // Add cache buster
  String url = firmwareUrl;
  if (url.indexOf('?') == -1) url += "?t=" + String(millis());
  else url += "&t=" + String(millis());

  ESPhttpUpdate.setLedPin(LED_BUILTIN, LOW);
  
  // This function will block until update is complete or fails
  t_httpUpdate_return ret = ESPhttpUpdate.update(client, url);
  
  if (ret == HTTP_UPDATE_FAILED) {
      display.clearDisplay();
      display.setCursor(0,0);
      display.println("Update Failed");
      display.println(ESPhttpUpdate.getLastErrorString());
      display.display();
      delay(5000);
      ESP.restart();
  }
}
