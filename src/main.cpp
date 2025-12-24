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
#include "icons.h"

// =============================================================================
// CONFIGURATION
// =============================================================================

const char *WIFI_SSID = "";
const char *WIFI_PASSWORD = "";
const char *LOCATION_POSTAL_CODE = "";
const char *ACCUWEATHER_API_KEY = "";
const bool USE_CELSIUS = false;  // Set to true for Celsius, false for Fahrenheit
const bool USE_24_HOUR = false;  // Set to true for 24-hour time, false for 12-hour

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

// =============================================================================
// STATE VARIABLES
// =============================================================================

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
// HELPER FUNCTIONS
// =============================================================================

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
  
  if (USE_24_HOUR)
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
    if (USE_CELSIUS)
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
    int highDisplay = USE_CELSIUS ? (int)round((forecast[i].highTemp - 32) * 5.0 / 9.0) : forecast[i].highTemp;
    sprintf(highStr, "%d%c", highDisplay, 247);
    int highWidth = strlen(highStr) * 12;  // 6 * 2 = 12 pixels per char
    int highX = colCenterX - (highWidth / 2);
    tft.setCursor(highX, startY + 58);
    tft.print(highStr);
    
    // Low temp (blue)
    tft.setTextColor(ST77XX_BLUE, ST77XX_BLACK);
    tft.setTextSize(2);
    char lowStr[8];
    int lowDisplay = USE_CELSIUS ? (int)round((forecast[i].lowTemp - 32) * 5.0 / 9.0) : forecast[i].lowTemp;
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
    if (USE_24_HOUR)
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

void connectToWiFi()
{
  displayCenteredText("Connecting to Earth...", ST77XX_CYAN);
  Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

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
  url += LOCATION_POSTAL_CODE;
  url += "&countryCode=US";

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
  delay(5000);

  Serial.println("\n\n================================");
  Serial.println("ESP32-C3 Weather Station");
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
  Wire.setClock(10000);  // 10kHz - lower frequency to reduce sensor self-heating
  delay(100);

  // Initialize display
  Serial.println("Initializing display...");
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.init(240, 280);
  tft.setRotation(3); // Landscape: 280x240
  tft.fillScreen(ST77XX_BLACK);
  Serial.println("Display ready");

  // Connect to WiFi
  connectToWiFi();

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