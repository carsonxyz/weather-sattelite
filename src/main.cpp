#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Adafruit_AHTX0.h>
#include <time.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include "icons.h"

// =============================================================================
// FIRMWARE VERSION (for OTA updates)
// =============================================================================
const char* FIRMWARE_VERSION = "1.0.0";

// =============================================================================
// CONFIGURATION (stored in NVS, set via captive portal)
// =============================================================================

// Hardcoded API key for all devices
const char *ACCUWEATHER_API_KEY = "";

// These are loaded from non-volatile storage
String cfg_wifiSsid = "";
String cfg_wifiPassword = "";
String cfg_postalCode = "";
String cfg_countryCode = "US";
bool cfg_useCelsius = false;
bool cfg_use24Hour = false;
bool configValid = false;

// Captive portal settings
const char *AP_SSID = "Satellite-Setup";
const char *AP_PASSWORD = "";  // Open network for easy setup

// =============================================================================
// PIN DEFINITIONS
// =============================================================================

// Display (ST7789 240x280, rotated to 280x240 landscape)
#define TFT_CS 10
#define TFT_DC 9
#define TFT_RST 8
#define TFT_MOSI 7
#define TFT_SCLK 6

// I2C (AHT10 sensor)
#define PIN_I2C_SDA 4
#define PIN_I2C_SCL 3

// Controls
#define PIN_TOUCH 2     // TTP223B capacitive touch
#define PIN_LIGHT_SW 21 // Light switch (LOW = on, HIGH = off)

// Outputs
#define PIN_BACKLIGHT 20 // Display backlight (PWM)
#define PIN_LED 0        // Notification LED (PWM)

// =============================================================================
// DISPLAY CONFIGURATION
// =============================================================================

#define SCREEN_W 280
#define SCREEN_H 240

// =============================================================================
// GLOBAL OBJECTS
// =============================================================================

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
Adafruit_AHTX0 aht;
Preferences preferences;
WebServer server(80);
DNSServer dnsServer;

// =============================================================================
// STATE VARIABLES
// =============================================================================

bool setupMode = false;  // True when in captive portal setup mode
bool ahtFound = false;
bool lightsEnabled = true;
bool lastTouchState = LOW;
bool touchHandled = false;

// Location data from AccuWeather
String LOCATION_KEY = "";
String TIME_ZONE = "";
float GMT_OFFSET_HOURS = 0;
bool IS_DST = false;

// Time display
unsigned long lastTimeUpdate = 0;
String lastTimeStr = "";
bool colonVisible = true;

// Screen state
int currentScreen = 1;  // 1 = screen one, 2 = screen two

// Sensor caching (to prevent self-heating from frequent polling)
unsigned long lastSensorRead = 0;
const unsigned long SENSOR_READ_INTERVAL = 30000;  // Read sensor every 30 seconds
float cachedTempF = 0;
float cachedHumidity = 0;
bool sensorDataValid = false;

// Forecast data
struct DayForecast {
  int iconNum;
  int highTemp;
  int lowTemp;
  String dayName;
};
DayForecast forecast[3];
bool forecastValid = false;
unsigned long lastForecastFetch = 0;
const unsigned long FORECAST_REFRESH_INTERVAL = 3600000;  // Refresh forecast every 1 hour

// =============================================================================
// CONFIGURATION STORAGE FUNCTIONS
// =============================================================================

void loadConfiguration()
{
  preferences.begin("weather", true);  // Read-only mode
  
  cfg_wifiSsid = preferences.getString("wifiSsid", "");
  cfg_wifiPassword = preferences.getString("wifiPass", "");
  cfg_postalCode = preferences.getString("postalCode", "");
  cfg_countryCode = preferences.getString("countryCode", "US");
  cfg_useCelsius = preferences.getBool("useCelsius", false);
  cfg_use24Hour = preferences.getBool("use24Hour", false);
  
  preferences.end();
  
  // Configuration is valid if we have the essentials
  configValid = (cfg_wifiSsid.length() > 0 && cfg_postalCode.length() > 0);
  
  Serial.printf("Configuration loaded: %s\n", configValid ? "Valid" : "Invalid/Empty");
  if (configValid)
  {
    Serial.printf("  WiFi SSID: %s\n", cfg_wifiSsid.c_str());
    Serial.printf("  Postal Code: %s\n", cfg_postalCode.c_str());
    Serial.printf("  Country: %s\n", cfg_countryCode.c_str());
    Serial.printf("  Celsius: %s\n", cfg_useCelsius ? "Yes" : "No");
    Serial.printf("  24-Hour: %s\n", cfg_use24Hour ? "Yes" : "No");
  }
}

void saveConfiguration()
{
  preferences.begin("weather", false);  // Read-write mode
  
  preferences.putString("wifiSsid", cfg_wifiSsid);
  preferences.putString("wifiPass", cfg_wifiPassword);
  preferences.putString("postalCode", cfg_postalCode);
  preferences.putString("countryCode", cfg_countryCode);
  preferences.putBool("useCelsius", cfg_useCelsius);
  preferences.putBool("use24Hour", cfg_use24Hour);
  
  preferences.end();
  
  configValid = true;
  Serial.println("Configuration saved!");
}

void clearConfiguration()
{
  preferences.begin("weather", false);
  preferences.clear();
  preferences.end();
  configValid = false;
  Serial.println("Configuration cleared!");
}

// =============================================================================
// CAPTIVE PORTAL HTML
// =============================================================================

const char SETUP_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Atmospheric Satellite</title>
  <style>
    * { box-sizing: border-box; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; }
    body { margin: 0; padding: 20px; background: #1a1a2e; color: #eee; min-height: 100vh; }
    .container { max-width: 400px; margin: 0 auto; }
    h1 { color: #00d4ff; text-align: center; margin-bottom: 30px; font-size: 24px; }
    h2 { color: #ff9f43; font-size: 16px; margin-top: 25px; margin-bottom: 10px; border-bottom: 1px solid #333; padding-bottom: 5px; }
    label { display: block; margin-bottom: 5px; color: #aaa; font-size: 14px; }
    input[type="text"], input[type="password"] { 
      width: 100%; padding: 12px; margin-bottom: 15px; 
      border: 1px solid #333; border-radius: 8px; 
      background: #16213e; color: #fff; font-size: 16px;
    }
    input:focus { outline: none; border-color: #00d4ff; }
    .checkbox-group { display: flex; align-items: center; margin-bottom: 15px; }
    .checkbox-group input { width: 20px; height: 20px; margin-right: 10px; }
    .checkbox-group label { margin-bottom: 0; }
    button { 
      width: 100%; padding: 15px; margin-top: 20px;
      background: #00d4ff; color: #000; border: none; 
      border-radius: 8px; font-size: 18px; font-weight: bold;
      cursor: pointer; transition: background 0.3s;
    }
    button:hover { background: #00a8cc; }
    .note { font-size: 12px; color: #666; margin-top: 5px; }
    .icon { font-size: 48px; text-align: center; margin-bottom: 10px; }
  </style>
</head>
<body>
  <div class="container">
    <h1>Atmospheric Satellite</h1>
    <form action="/save" method="POST">
      <h2>WiFi Settings</h2>
      <label>WiFi Network Name (SSID)</label>
      <input type="text" name="ssid" required placeholder="Your WiFi network">
      <label>WiFi Password</label>
      <input type="password" name="password" placeholder="WiFi password">
      
      <h2>Location</h2>
      <label>Postal/ZIP Code</label>
      <input type="text" name="postal" required placeholder="e.g., 90210 or M5V 2E1">
      <label>Country Code</label>
      <input type="text" name="country" value="US" maxlength="2" placeholder="e.g., US, CA, UK">
      <p class="note">2 digit country code</p>
      
      <h2>Display Preferences</h2>
      <div class="checkbox-group">
        <input type="checkbox" id="celsius" name="celsius" value="1">
        <label for="celsius">Use Celsius (instead of Fahrenheit)</label>
      </div>
      <div class="checkbox-group">
        <input type="checkbox" id="hour24" name="hour24" value="1">
        <label for="hour24">Use 24-hour time format</label>
      </div>
      
      <button type="submit">Save & Connect</button>
    </form>
  </div>
</body>
</html>
)rawliteral";

const char SAVE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Settings Saved</title>
  <style>
    * { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; }
    body { margin: 0; padding: 20px; background: #1a1a2e; color: #eee; min-height: 100vh; 
           display: flex; align-items: center; justify-content: center; text-align: center; }
    .container { max-width: 400px; }
    h1 { color: #00d4ff; }
    p { color: #aaa; line-height: 1.6; }
    .icon { font-size: 64px; margin-bottom: 20px; }
  </style>
</head>
<body>
  <div class="container">
    <div class="icon">✅</div>
    <h1>Settings Saved!</h1>
    <p>Your atmospheric satellite is now configured.<br>The device will restart and connect to your WiFi network.</p>
    <p style="color: #666; font-size: 14px; margin-top: 30px;">
      To reconfigure later, hold the touch button while powering on the device.
    </p>
  </div>
</body>
</html>
)rawliteral";

// =============================================================================
// CAPTIVE PORTAL HANDLERS
// =============================================================================

void handleRoot()
{
  server.send(200, "text/html", SETUP_HTML);
}

void handleSave()
{
  cfg_wifiSsid = server.arg("ssid");
  cfg_wifiPassword = server.arg("password");
  cfg_postalCode = server.arg("postal");
  cfg_countryCode = server.arg("country");
  cfg_useCelsius = server.hasArg("celsius");
  cfg_use24Hour = server.hasArg("hour24");
  
  if (cfg_countryCode.length() == 0) cfg_countryCode = "US";
  
  saveConfiguration();
  
  server.send(200, "text/html", SAVE_HTML);
  
  // Wait a moment for the response to be sent, then restart
  delay(3000);
  ESP.restart();
}

void handleNotFound()
{
  // Redirect all requests to the setup page (captive portal behavior)
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

void startCaptivePortal()
{
  setupMode = true;
  Serial.println("\n=== Starting Captive Portal ===");
  
  // Display setup message
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(2);
  
  tft.setCursor(50, 40);
  tft.print("Satellite setup");
  
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(20, 80);
  tft.print("Connect to WiFi:");
  
  tft.setTextColor(ST77XX_ORANGE);
  tft.setTextSize(2);
  tft.setCursor(20, 110);
  tft.print(AP_SSID);
  
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(20, 150);
  tft.print("On your smartphone");
  tft.setCursor(20, 175);
  tft.print("to configure.");
  
  // Start Access Point
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  
  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("AP IP address: %s\n", apIP.toString().c_str());
  
  // Start DNS server (redirect all domains to our IP)
  dnsServer.start(53, "*", apIP);
  
  // Setup web server routes
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  
  server.begin();
  Serial.println("Web server started");
  Serial.printf("Connect to WiFi '%s' and open any webpage\n", AP_SSID);
}

void runCaptivePortalLoop()
{
  dnsServer.processNextRequest();
  server.handleClient();
}

// =============================================================================
// HELPER FUNCTIONS
// =============================================================================

// URL encode a string (handles spaces and special characters)
String urlEncode(const String &str)
{
  String encoded = "";
  char c;
  for (int i = 0; i < str.length(); i++)
  {
    c = str.charAt(i);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
    {
      encoded += c;
    }
    else if (c == ' ')
    {
      encoded += "%20";
    }
    else
    {
      char buf[4];
      sprintf(buf, "%%%02X", (unsigned char)c);
      encoded += buf;
    }
  }
  return encoded;
}

// Map AccuWeather icon number to local bitmap
const unsigned char* getWeatherIcon(int iconNum)
{
  // AccuWeather icon mapping:
  // 1-2: Sunny/Mostly sunny -> sunny
  // 3-5: Partly sunny/Intermittent clouds/Hazy -> partly_cloudy
  // 6-8: Mostly cloudy/Cloudy/Dreary -> cloudy
  // 11: Fog -> foggy
  // 12-14, 18, 26, 29: Showers/Rain/Freezing rain/Rain+snow -> rainy
  // 15-17: Thunderstorms -> thunderstorm
  // 19-25: Flurries/Snow/Ice/Sleet -> snowing
  // 30-31: Hot/Cold -> sunny (closest match)
  // 32: Windy -> cloudy
  
  switch (iconNum) {
    case 1:
    case 2:
    case 30:
    case 31:
      return weather_sunny;
    case 3:
    case 4:
    case 5:
      return weather_partly_cloudy;
    case 6:
    case 7:
    case 8:
    case 32:
      return weather_cloudy;
    case 11:
      return weather_foggy;
    case 12:
    case 13:
    case 14:
    case 18:
    case 26:
    case 29:
      return weather_rainy;
    case 15:
    case 16:
    case 17:
      return weather_thunderstorm;
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 25:
      return weather_snowing;
    default:
      return weather_partly_cloudy;  // Default fallback
  }
}

void displayCenteredText(const char *text, uint16_t color)
{
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(color);
  tft.setTextSize(2);

  const int padding = 20;
  const int charWidth = 12;  // 6 pixels per char at size 2 * 2
  const int lineHeight = 20; // 16 pixels tall + spacing
  const int maxWidth = SCREEN_W - (padding * 2);
  const int maxCharsPerLine = maxWidth / charWidth;

  // Split text into lines
  String inputText = String(text);
  String lines[4]; // Max 4 lines
  int lineCount = 0;

  while (inputText.length() > 0 && lineCount < 4)
  {
    if ((int)inputText.length() <= maxCharsPerLine)
    {
      // Remaining text fits on one line
      lines[lineCount++] = inputText;
      break;
    }
    else
    {
      // Find last space within maxCharsPerLine
      int breakPoint = maxCharsPerLine;
      for (int i = maxCharsPerLine; i >= 0; i--)
      {
        if (inputText.charAt(i) == ' ')
        {
          breakPoint = i;
          break;
        }
      }

      lines[lineCount++] = inputText.substring(0, breakPoint);
      inputText = inputText.substring(breakPoint + 1); // Skip the space
    }
  }

  // Calculate total height and starting Y position
  int totalHeight = lineCount * lineHeight;
  int startY = (SCREEN_H - totalHeight) / 2;

  // Draw each line centered
  for (int i = 0; i < lineCount; i++)
  {
    int textWidth = lines[i].length() * charWidth;
    int x = (SCREEN_W - textWidth) / 2;
    int y = startY + (i * lineHeight);

    tft.setCursor(x, y);
    tft.println(lines[i]);
  }
}

void displayTime()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    return;
  }

  // Format time string based on 12/24 hour setting
  char baseTimeStr[16];
  char timeStr[16];
  
  if (cfg_use24Hour)
  {
    sprintf(baseTimeStr, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    if (colonVisible)
    {
      sprintf(timeStr, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    }
    else
    {
      sprintf(timeStr, "%02d %02d", timeinfo.tm_hour, timeinfo.tm_min);
    }
  }
  else
  {
    int hour12 = timeinfo.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    const char *ampm = (timeinfo.tm_hour < 12) ? "AM" : "PM";
    sprintf(baseTimeStr, "%d:%02d %s", hour12, timeinfo.tm_min, ampm);
    if (colonVisible)
    {
      sprintf(timeStr, "%d:%02d %s", hour12, timeinfo.tm_min, ampm);
    }
    else
    {
      sprintf(timeStr, "%d %02d %s", hour12, timeinfo.tm_min, ampm);
    }
  }
  colonVisible = !colonVisible;  // Toggle for next update

  // Check if time actually changed (not just colon blink)
  bool timeChanged = (String(baseTimeStr) != lastTimeStr);

  // Calculate centered position
  int charWidth = 6 * 5;  // 6 pixels per char * text size 5
  int textWidth = strlen(timeStr) * charWidth;
  int x = (SCREEN_W - textWidth) / 2;
  int y = (SCREEN_H - 40) / 2;  // 8 pixels tall * 5 = 40

  // Only clear screen if time changed, not just colon blink
  if (timeChanged)
  {
    tft.fillScreen(ST77XX_BLACK);
    lastTimeStr = String(baseTimeStr);
  }

  // Use setTextColor with background to overwrite without flicker
  tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
  tft.setTextSize(5);  // Large font
  tft.setCursor(x, y);
  tft.print(timeStr);
}

void displayTempHum()
{
  // Display temperature and humidity from AHT10
  if (ahtFound)
  {
    // Only read sensor every SENSOR_READ_INTERVAL ms to prevent self-heating
    if (!sensorDataValid || (millis() - lastSensorRead >= SENSOR_READ_INTERVAL))
    {
      sensors_event_t humidity, temp;
      aht.getEvent(&humidity, &temp);
      
      // Calibration offset (in Fahrenheit)
      const float TEMP_OFFSET_F = -6.0;  // Calibration: sensor reads ~6°F high
      cachedTempF = (temp.temperature * 9.0 / 5.0) + 32.0 + TEMP_OFFSET_F;
      cachedHumidity = humidity.relative_humidity;
      
      lastSensorRead = millis();
      sensorDataValid = true;
      
      Serial.printf("Sensor read: %.1f°F, %.1f%%\n", cachedTempF, cachedHumidity);
    }
    
    // Format strings using cached values
    char tempStr[16];
    char humStr[16];
    if (cfg_useCelsius)
    {
      float tempC = (cachedTempF - 32.0) * 5.0 / 9.0;
      sprintf(tempStr, "Temp: %.0f%cC", tempC, 247);  // 247 is degree symbol
    }
    else
    {
      sprintf(tempStr, "Temp: %.0f%cF", cachedTempF, 247);  // 247 is degree symbol
    }
    sprintf(humStr, "Hum: %.0f%%", cachedHumidity);
    
    tft.setTextColor(ST77XX_ORANGE, ST77XX_BLACK);
    tft.setTextSize(2);  // Small text
    
    int charWidth = 6 * 2;  // 6 pixels per char * text size 2
    int y = SCREEN_H - 40;  // Bottom of screen with some padding
    
    // Draw horizontal line above temp/humidity
    tft.drawFastHLine(0, y - 12, SCREEN_W, ST77XX_ORANGE);
    
    // Left column - Temperature (centered in left half)
    int tempWidth = strlen(tempStr) * charWidth;
    int tempX = (SCREEN_W / 4) - (tempWidth / 2);
    tft.setCursor(tempX, y);
    tft.print(tempStr);
    
    // Right column - Humidity (centered in right half)
    int humWidth = strlen(humStr) * charWidth;
    int humX = (SCREEN_W * 3 / 4) - (humWidth / 2);
    tft.setCursor(humX, y);
    tft.print(humStr);
  }
}

void displayScreenOne()
{
  displayTime();
  displayTempHum();
  
  // Draw satellite icon in upper left corner (32x32)
  tft.drawBitmap(20, 20, weather_satellite, 32, 32, ST77XX_CYAN);
}

void displayScreenTwo()
{
  tft.fillScreen(ST77XX_BLACK);
  
  if (!forecastValid)
  {
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(2);
    const char* text = "Loading forecast...";
    int charWidth = 6 * 2;
    int textWidth = strlen(text) * charWidth;
    int x = (SCREEN_W - textWidth) / 2;
    int y = (SCREEN_H - 16) / 2;
    tft.setCursor(x, y);
    tft.print(text);
    return;
  }
  
  // Display 3-day forecast in 3 columns
  // Screen is 280x240
  // Each column: ~93px wide
  int colWidth = SCREEN_W / 3;
  int iconSize = 48;  // Weather icons are 48x48
  
  // Content height: icon (48) + gap (10) + high temp (16) + gap (8) + low temp (16) = 98
  int contentHeight = 98;
  int startY = (SCREEN_H - contentHeight) / 2;
  
  for (int i = 0; i < 3; i++)
  {
    int colCenterX = (i * colWidth) + (colWidth / 2);
    
    // Weather icon centered in column (48x48)
    const unsigned char* icon = getWeatherIcon(forecast[i].iconNum);
    int iconX = colCenterX - (iconSize / 2);
    tft.drawBitmap(iconX, startY, icon, iconSize, iconSize, ST77XX_WHITE);
    
    // High temp (orange)
    tft.setTextColor(ST77XX_ORANGE, ST77XX_BLACK);
    tft.setTextSize(2);
    char highStr[8];
    int highDisplay = cfg_useCelsius ? (int)round((forecast[i].highTemp - 32) * 5.0 / 9.0) : forecast[i].highTemp;
    sprintf(highStr, "%d%c", highDisplay, 247);
    int highWidth = strlen(highStr) * 12;  // 6 * 2 = 12 pixels per char
    int highX = colCenterX - (highWidth / 2);
    tft.setCursor(highX, startY + 58);
    tft.print(highStr);
    
    // Low temp (blue)
    tft.setTextColor(ST77XX_BLUE, ST77XX_BLACK);
    tft.setTextSize(2);
    char lowStr[8];
    int lowDisplay = cfg_useCelsius ? (int)round((forecast[i].lowTemp - 32) * 5.0 / 9.0) : forecast[i].lowTemp;
    sprintf(lowStr, "%d%c", lowDisplay, 247);
    int lowWidth = strlen(lowStr) * 12;
    int lowX = colCenterX - (lowWidth / 2);
    tft.setCursor(lowX, startY + 82);
    tft.print(lowStr);
  }
  
  // Display current time at bottom
  struct tm timeinfo;
  if (getLocalTime(&timeinfo))
  {
    char timeStr[16];
    if (cfg_use24Hour)
    {
      sprintf(timeStr, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    }
    else
    {
      int hour12 = timeinfo.tm_hour % 12;
      if (hour12 == 0) hour12 = 12;
      const char *ampm = (timeinfo.tm_hour < 12) ? "AM" : "PM";
      sprintf(timeStr, "%d:%02d %s", hour12, timeinfo.tm_min, ampm);
    }
    
    tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
    tft.setTextSize(2);
    int timeWidth = strlen(timeStr) * 12;
    int timeX = (SCREEN_W - timeWidth) / 2;
    tft.setCursor(timeX, SCREEN_H - 30);
    tft.print(timeStr);
  }
}

// =============================================================================
// OTA UPDATE FUNCTIONS
// =============================================================================

// OTA Update URLs (GitHub Releases)
const char* OTA_VERSION_URL = "https://github.com/carsonxyz/weather-satellite/releases/latest/download/version.txt";
const char* OTA_FIRMWARE_URL = "https://github.com/carsonxyz/weather-satellite/releases/latest/download/firmware.bin";
const int OTA_TIMEOUT_MS = 30000;  // 30 second timeout for downloads

// Parse semantic version string "major.minor.patch" into components
// Returns true if parsing succeeded
bool parseVersion(const char* versionStr, int& major, int& minor, int& patch)
{
  major = minor = patch = 0;

  // Copy string to allow modification
  char buf[32];
  strncpy(buf, versionStr, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  // Trim leading/trailing whitespace and newlines
  char* start = buf;
  while (*start && (isspace(*start) || *start == '\n' || *start == '\r')) start++;
  char* end = start + strlen(start) - 1;
  while (end > start && (isspace(*end) || *end == '\n' || *end == '\r')) *end-- = '\0';

  // Skip 'v' prefix if present
  if (*start == 'v' || *start == 'V') start++;

  // Parse major.minor.patch
  char* token = strtok(start, ".");
  if (!token) return false;
  major = atoi(token);

  token = strtok(NULL, ".");
  if (!token) return false;
  minor = atoi(token);

  token = strtok(NULL, ".-");  // Stop at dash for pre-release tags
  if (!token) return false;
  patch = atoi(token);

  return true;
}

// Compare two semantic versions
// Returns: 1 if v1 > v2, -1 if v1 < v2, 0 if equal
int compareVersions(const char* v1, const char* v2)
{
  int major1, minor1, patch1;
  int major2, minor2, patch2;

  if (!parseVersion(v1, major1, minor1, patch1)) return 0;
  if (!parseVersion(v2, major2, minor2, patch2)) return 0;

  if (major1 != major2) return (major1 > major2) ? 1 : -1;
  if (minor1 != minor2) return (minor1 > minor2) ? 1 : -1;
  if (patch1 != patch2) return (patch1 > patch2) ? 1 : -1;

  return 0;
}

// Check for and perform OTA firmware updates
// Call this after WiFi is connected
void checkForUpdates()
{
  Serial.println("\n--- Checking for Firmware Updates ---");
  Serial.printf("Current firmware version: %s\n", FIRMWARE_VERSION);

  displayCenteredText("Checking for updates...", ST77XX_CYAN);

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi not connected, skipping update check");
    return;
  }

  // Mark current firmware as valid (for rollback protection)
  // This should be called after the firmware has been verified to work
  esp_ota_mark_app_valid_cancel_rollback();

  HTTPClient http;

  // Configure for HTTPS with GitHub
  // NOTE: Using insecure mode for simplicity. For production, add GitHub's root CA certificate.
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(OTA_TIMEOUT_MS);

  // Fetch version.txt from GitHub releases
  Serial.printf("Fetching version from: %s\n", OTA_VERSION_URL);
  http.begin(OTA_VERSION_URL);

  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK)
  {
    Serial.printf("Failed to fetch version file. HTTP code: %d\n", httpCode);
    http.end();
    Serial.println("Update check failed, continuing with current firmware");
    return;
  }

  String remoteVersion = http.getString();
  remoteVersion.trim();
  http.end();

  Serial.printf("Remote version: %s\n", remoteVersion.c_str());

  // Compare versions
  int cmp = compareVersions(remoteVersion.c_str(), FIRMWARE_VERSION);

  if (cmp <= 0)
  {
    Serial.println("Firmware is up to date");
    Serial.println("--- Update Check Complete ---\n");
    return;
  }

  // Remote version is newer - perform update
  Serial.printf("New version available: %s -> %s\n", FIRMWARE_VERSION, remoteVersion.c_str());

  displayCenteredText("Updating firmware...", ST77XX_CYAN);

  Serial.printf("Downloading firmware from: %s\n", OTA_FIRMWARE_URL);

  // Configure HTTPUpdate
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  // Create WiFiClient for HTTP (GitHub redirects to objects.githubusercontent.com)
  WiFiClientSecure client;
  client.setInsecure();  // Skip certificate verification
  // NOTE: For production, you should add proper certificate validation:
  // client.setCACert(github_root_ca);

  // Perform the update
  t_httpUpdate_return ret = httpUpdate.update(client, OTA_FIRMWARE_URL);

  switch (ret)
  {
    case HTTP_UPDATE_FAILED:
      Serial.printf("Update failed. Error (%d): %s\n",
                    httpUpdate.getLastError(),
                    httpUpdate.getLastErrorString().c_str());
      displayCenteredText("Update failed", ST77XX_RED);
      delay(3000);
      break;

    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("No update available");
      break;

    case HTTP_UPDATE_OK:
      Serial.println("Update successful! Rebooting...");
      // Device will reboot automatically after successful update
      break;
  }

  Serial.println("--- Update Check Complete ---\n");
}

void connectToWiFi()
{
  displayCenteredText("Connecting to Earth...", ST77XX_CYAN);
  Serial.printf("Connecting to WiFi: %s\n", cfg_wifiSsid.c_str());

  WiFi.begin(cfg_wifiSsid.c_str(), cfg_wifiPassword.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20)
  {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    displayCenteredText("Connected to Earth", ST77XX_CYAN);
    Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  }
  else
  {
    displayCenteredText("Could not connect to Earth", ST77XX_CYAN);
    Serial.println("WiFi connection failed");
  }

  delay(2000);
}

void fetchAccuWeatherLocation()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi not connected, skipping AccuWeather fetch");
    return;
  }

  Serial.println("\n--- Fetching AccuWeather Location Data ---");

  HTTPClient http;

  // Build the AccuWeather Location API URL
  String url = "https://dataservice.accuweather.com/locations/v1/postalcodes/search?q=";
  url += urlEncode(cfg_postalCode);
  url += "&countryCode=";
  url += cfg_countryCode;

  Serial.printf("Request URL: %s\n", url.c_str());

  http.begin(url);
  http.addHeader("Accept", "application/json");
  http.addHeader("Authorization", String("Bearer ") + ACCUWEATHER_API_KEY);

  int httpCode = http.GET();

  if (httpCode > 0)
  {
    Serial.printf("HTTP Response Code: %d\n", httpCode);
    String payload = http.getString();

    if (httpCode == HTTP_CODE_OK)
    {
      Serial.println("AccuWeather Response:");
      Serial.println(payload);

      // Parse JSON response
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload);

      if (error)
      {
        Serial.printf("JSON parsing failed: %s\n", error.c_str());
      }
      else
      {
        // Response is an array, get first element
        JsonObject location = doc[0];

        if (!location.isNull())
        {
          LOCATION_KEY = location["Key"].as<String>();
          TIME_ZONE = location["TimeZone"]["Name"].as<String>();
          GMT_OFFSET_HOURS = location["TimeZone"]["GmtOffset"].as<float>();
          IS_DST = location["TimeZone"]["IsDaylightSaving"].as<bool>();

          Serial.printf("Location Key: %s\n", LOCATION_KEY.c_str());
          Serial.printf("Time Zone: %s\n", TIME_ZONE.c_str());
          Serial.printf("GMT Offset: %.1f hours\n", GMT_OFFSET_HOURS);
          Serial.printf("Daylight Saving: %s\n", IS_DST ? "Yes" : "No");
        }
        else
        {
          Serial.println("No location data found in response");
        }
      }
    }
    else
    {
      Serial.println("AccuWeather Error Response:");
      Serial.println(payload);
    }
  }
  else
  {
    Serial.printf("HTTP Request failed: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
  Serial.println("--- AccuWeather Fetch Complete ---\n");
}

void syncTimeWithNTP()
{
  if (TIME_ZONE.length() == 0)
  {
    Serial.println("No timezone set, skipping NTP sync");
    return;
  }

  Serial.println("\n--- Syncing Time with NTP ---");
  Serial.printf("Using timezone: %s\n", TIME_ZONE.c_str());

  // Convert GMT offset from hours to seconds
  // The IsDaylightSaving flag from AccuWeather tells us if DST is currently active
  // The GmtOffset already accounts for DST, so we don't add additional DST offset
  long gmtOffsetSec = (long)(GMT_OFFSET_HOURS * 3600);
  int daylightOffsetSec = 0;  // GmtOffset already includes DST adjustment
  
  Serial.printf("GMT Offset: %ld seconds\n", gmtOffsetSec);

  // Configure NTP with timezone offset
  // Using pool.ntp.org as the NTP server
  configTime(gmtOffsetSec, daylightOffsetSec, "pool.ntp.org", "time.nist.gov");

  // Wait for time to be set
  Serial.print("Waiting for NTP time sync");
  int attempts = 0;
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo) && attempts < 10)
  {
    Serial.print(".");
    delay(500);
    attempts++;
  }
  Serial.println();

  if (getLocalTime(&timeinfo))
  {
    Serial.println("Time synchronized!");
    Serial.printf("Current time: %04d-%02d-%02d %02d:%02d:%02d\n",
                  timeinfo.tm_year + 1900,
                  timeinfo.tm_mon + 1,
                  timeinfo.tm_mday,
                  timeinfo.tm_hour,
                  timeinfo.tm_min,
                  timeinfo.tm_sec);
  }
  else
  {
    Serial.println("Failed to sync time with NTP");
  }

  Serial.println("--- NTP Sync Complete ---\n");
}

void fetchForecast()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi not connected, skipping forecast fetch");
    return;
  }

  if (LOCATION_KEY.length() == 0)
  {
    Serial.println("No location key, skipping forecast fetch");
    return;
  }

  // Check if we need to refresh (every hour)
  if (forecastValid && (millis() - lastForecastFetch < FORECAST_REFRESH_INTERVAL))
  {
    Serial.println("Forecast still fresh, skipping fetch");
    return;
  }

  Serial.println("\n--- Fetching 5-Day Forecast ---");

  HTTPClient http;

  // Build the AccuWeather Forecast API URL
  String url = "https://dataservice.accuweather.com/forecasts/v1/daily/5day/";
  url += LOCATION_KEY;

  Serial.printf("Request URL: %s\n", url.c_str());

  http.begin(url);
  http.addHeader("Accept", "application/json");
  http.addHeader("Authorization", String("Bearer ") + ACCUWEATHER_API_KEY);

  int httpCode = http.GET();

  if (httpCode > 0)
  {
    Serial.printf("HTTP Response Code: %d\n", httpCode);
    String payload = http.getString();

    if (httpCode == HTTP_CODE_OK)
    {
      Serial.println("Forecast received, parsing...");

      // Parse JSON response
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload);

      if (error)
      {
        Serial.printf("JSON parsing failed: %s\n", error.c_str());
      }
      else
      {
        JsonArray dailyForecasts = doc["DailyForecasts"];
        
        // Get first 3 days
        for (int i = 0; i < 3 && i < dailyForecasts.size(); i++)
        {
          JsonObject day = dailyForecasts[i];
          
          // Get day icon number (use Day icon, not Night)
          forecast[i].iconNum = day["Day"]["Icon"].as<int>();
          
          // Get high/low temps (already in Fahrenheit from API)
          forecast[i].highTemp = (int)round(day["Temperature"]["Maximum"]["Value"].as<float>());
          forecast[i].lowTemp = (int)round(day["Temperature"]["Minimum"]["Value"].as<float>());
          
          // Parse date to get day name
          String dateStr = day["Date"].as<String>();
          // Date format: "2024-01-15T07:00:00-05:00"
          // Extract year, month, day
          int year = dateStr.substring(0, 4).toInt();
          int month = dateStr.substring(5, 7).toInt();
          int dayNum = dateStr.substring(8, 10).toInt();
          
          // Calculate day of week using Zeller's formula (simplified)
          struct tm tm = {0};
          tm.tm_year = year - 1900;
          tm.tm_mon = month - 1;
          tm.tm_mday = dayNum;
          mktime(&tm);
          
          const char* dayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
          forecast[i].dayName = String(dayNames[tm.tm_wday]);
          
          Serial.printf("Day %d: %s - Icon:%d High:%d Low:%d\n", 
                        i, forecast[i].dayName.c_str(), 
                        forecast[i].iconNum, forecast[i].highTemp, forecast[i].lowTemp);
        }
        
        forecastValid = true;
        lastForecastFetch = millis();
        Serial.println("Forecast parsed successfully!");
      }
    }
    else
    {
      Serial.println("AccuWeather Forecast Error:");
      Serial.println(payload);
    }
  }
  else
  {
    Serial.printf("HTTP Request failed: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
  Serial.println("--- Forecast Fetch Complete ---\n");
}

// =============================================================================
// SETUP
// =============================================================================

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n\n================================");
  Serial.println("ESP32-C3 Atmospheric Satellite");
  Serial.printf("Firmware Version: %s\n", FIRMWARE_VERSION);
  Serial.println("================================");

  // Initialize outputs
  pinMode(PIN_BACKLIGHT, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  analogWrite(PIN_BACKLIGHT, 255);
  analogWrite(PIN_LED, 0);

  // Initialize inputs
  pinMode(PIN_TOUCH, INPUT);
  pinMode(PIN_LIGHT_SW, INPUT_PULLUP);

  // Initialize I2C first (before any I2C devices)
  Serial.println("Initializing I2C...");
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(1000);  // 1kHz - very low frequency to minimize sensor self-heating
  delay(100);

  // Initialize display
  Serial.println("Initializing display...");
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.init(240, 280);
  tft.setRotation(3); // Landscape: 280x240
  tft.fillScreen(ST77XX_BLACK);
  Serial.println("Display ready");

  // Load configuration from NVS
  loadConfiguration();

  // Check if touch button is held on boot to force setup mode
  delay(100);  // Debounce
  bool forceSetup = (digitalRead(PIN_TOUCH) == HIGH);
  if (forceSetup)
  {
    Serial.println("Touch button held - forcing setup mode");
    // Wait for button release
    while (digitalRead(PIN_TOUCH) == HIGH) delay(10);
  }

  // Enter setup mode if no config or touch button held
  if (!configValid || forceSetup)
  {
    Serial.println("Entering setup mode...");
    startCaptivePortal();
    return;  // Exit setup, loop will handle captive portal
  }

  // Normal boot - connect to WiFi
  connectToWiFi();

  // Check for OTA firmware updates after WiFi connection
  checkForUpdates();

  // Initialize AHT10 sensor
  Serial.println("Initializing AHT10...");
  if (aht.begin())
  {
    ahtFound = true;
    Serial.println("AHT10 sensor ready");
  }
  else
  {
    Serial.println("AHT10 sensor not found");
  }

  // Read initial switch state
  lightsEnabled = (digitalRead(PIN_LIGHT_SW) == LOW);
  Serial.printf("Light switch: %s\n", lightsEnabled ? "ON" : "OFF");

  // Fetch AccuWeather location data and sync time
  fetchAccuWeatherLocation();
  syncTimeWithNTP();
  fetchForecast();

  // Clear display and show initial screen
  tft.fillScreen(ST77XX_BLACK);
  displayScreenOne();

  Serial.println("Setup complete\n");
}

// =============================================================================
// MAIN LOOP
// =============================================================================

void loop()
{
  // If in setup mode, handle captive portal
  if (setupMode)
  {
    runCaptivePortalLoop();
    delay(10);
    return;
  }

  // --- Update display every second ---
  if (millis() - lastTimeUpdate >= 1000)
  {
    lastTimeUpdate = millis();
    if (lightsEnabled)
    {
      if (currentScreen == 1)
      {
        displayScreenOne();
      }
      // Screen two doesn't need constant updates
    }
  }

  // --- Check light switch ---
  bool switchState = digitalRead(PIN_LIGHT_SW);
  bool newLightsEnabled = (switchState == LOW);

  if (newLightsEnabled != lightsEnabled)
  {
    lightsEnabled = newLightsEnabled;
    Serial.printf("Light switch changed: %s\n", lightsEnabled ? "ON" : "OFF");

    if (lightsEnabled)
    {
      analogWrite(PIN_BACKLIGHT, 255);
    }
    else
    {
      analogWrite(PIN_BACKLIGHT, 0);
      analogWrite(PIN_LED, 0);
    }
  }

  // --- Check touch button ---
  bool touchState = digitalRead(PIN_TOUCH);

  if (touchState == HIGH && lastTouchState == LOW && !touchHandled)
  {
    Serial.println("Touch detected");
    touchHandled = true;

    // Toggle between screens
    if (currentScreen == 1)
    {
      currentScreen = 2;
      fetchForecast();  // Refresh forecast if needed when switching to screen 2
      displayScreenTwo();
    }
    else
    {
      currentScreen = 1;
      lastTimeStr = "";  // Force time redraw
      tft.fillScreen(ST77XX_BLACK);
      displayScreenOne();
    }
    
    // Blink LED 3 times at 25% brightness (only if lights enabled)
    if (lightsEnabled)
    {
      for (int i = 0; i < 3; i++)
      {
        analogWrite(PIN_LED, 64);  // 25% brightness
        delay(100);
        analogWrite(PIN_LED, 0);
        delay(250);
      }
    }
  }

  if (touchState == LOW && lastTouchState == LOW)
  {
    touchHandled = false;
  }

  lastTouchState = touchState;

  delay(10);
}