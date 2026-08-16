#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>

// Wi-Fi Credentials
const char* ssid = "Infinix";
const char* password = "1234567890";

// Public MQTT Broker Configuration
const char* mqtt_server = "broker.emqx.io";
const int mqtt_port = 1883;
const char* mqtt_topic = "arpan_weather_station/state";

// OLED Configuration
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Network Clients
WiFiClient espClient;
PubSubClient client(espClient);

// IST Timezone Constants: UTC +5:30 (19800 seconds, 0 DST)
const long IST_OFFSET_SEC = 19800;
const int DST_OFFSET_SEC = 0;

// Location Variables
String city = "Detecting...";
String country = "";
float latitude = 0.0;
float longitude = 0.0;

// Weather Telemetry Variables
float temp = 0.0;
float feelsLike = 0.0;
int humidity = 0;
float pressure = 0.0;
float windSpeed = 0.0;
int windDirection = 0;
int cloudCover = 0;
float uvIndex = 0.0;
int weatherCode = 0;
String conditionText = "Unknown";

// Air Quality Telemetry Variables
int usAqi = 0;
float pm2_5 = 0.0;
float pm10 = 0.0;
String aqiStatus = "Good";

unsigned long lastWeatherFetch = 0;
const unsigned long weatherInterval = 30000; // Update weather every 30 seconds

String getWeatherDescription(int code) {
  switch (code) {
    case 0: return "Clear Sky";
    case 1: return "Mainly Clear";
    case 2: return "Partly Cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Foggy";
    case 51: case 53: case 55: return "Drizzle";
    case 61: case 63: case 65: return "Rain";
    case 71: case 73: case 75: return "Snow";
    case 80: case 81: case 82: return "Rain Showers";
    case 95: return "Thunderstorm";
    default: return "Cloudy";
  }
}

String getAqiCategory(int aqi) {
  if (aqi <= 50) return "Good";
  if (aqi <= 100) return "Moderate";
  if (aqi <= 150) return "Sensitive";
  if (aqi <= 200) return "Unhealthy";
  if (aqi <= 300) return "Very Unhealthy";
  return "Hazardous";
}

// 1. Auto-detect Lat/Lon and City via ISP IP
bool fetchLocation() {
  WiFiClient clientHttp;
  HTTPClient http;
  http.begin(clientHttp, "http://ip-api.com/json/?fields=status,city,country,lat,lon");
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, payload);

    if (doc["status"] == "success") {
      city = doc["city"].as<String>();
      country = doc["country"].as<String>();
      latitude = doc["lat"].as<float>();
      longitude = doc["lon"].as<float>();
      http.end();
      return true;
    }
  }
  http.end();
  return false;
}

// 2. Fetch Meteorological & Air Quality Data
void fetchWeatherData() {
  if (latitude == 0.0 && longitude == 0.0) return;

  WiFiClientSecure clientSecure;
  clientSecure.setInsecure();
  HTTPClient https;

  // Weather Endpoint
  String weatherUrl = "https://api.open-meteo.com/v1/forecast?latitude=" + String(latitude, 4) +
                      "&longitude=" + String(longitude, 4) +
                      "&current=temperature_2m,relative_humidity_2m,apparent_temperature,surface_pressure,wind_speed_10m,wind_direction_10m,cloud_cover,uv_index,weather_code";

  if (https.begin(clientSecure, weatherUrl)) {
    if (https.GET() == HTTP_CODE_OK) {
      DynamicJsonDocument doc(2048);
      deserializeJson(doc, https.getString());
      JsonObject current = doc["current"];
      temp = current["temperature_2m"];
      feelsLike = current["apparent_temperature"];
      humidity = current["relative_humidity_2m"];
      pressure = current["surface_pressure"];
      windSpeed = current["wind_speed_10m"];
      windDirection = current["wind_direction_10m"];
      cloudCover = current["cloud_cover"];
      uvIndex = current["uv_index"];
      weatherCode = current["weather_code"];
      conditionText = getWeatherDescription(weatherCode);
    }
    https.end();
  }

  // Air Quality Endpoint
  String aqiUrl = "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=" + String(latitude, 4) +
                  "&longitude=" + String(longitude, 4) +
                  "&current=us_aqi,pm10,pm2_5";

  if (https.begin(clientSecure, aqiUrl)) {
    if (https.GET() == HTTP_CODE_OK) {
      DynamicJsonDocument doc(1024);
      deserializeJson(doc, https.getString());
      JsonObject currentAqi = doc["current"];
      usAqi = currentAqi["us_aqi"];
      pm2_5 = currentAqi["pm2_5"];
      pm10 = currentAqi["pm10"];
      aqiStatus = getAqiCategory(usAqi);
    }
    https.end();
  }
}

// 3. Format Local IST Time & Date
void getFormattedTime(char* timeBuffer, char* dateBuffer) {
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);
  strftime(timeBuffer, 12, "%I:%M:%S %p", timeinfo); // 12-hour format with AM/PM (e.g. 03:05:14 PM)
  strftime(dateBuffer, 12, "%d-%b-%Y", timeinfo);    // e.g. 16-Aug-2026
}

// 4. Publish Full Telemetry to Cloud MQTT
void publishTelemetry() {
  char timeBuf[12], dateBuf[12];
  getFormattedTime(timeBuf, dateBuf);

  DynamicJsonDocument doc(768);
  doc["city"] = city;
  doc["country"] = country;
  doc["time"] = String(timeBuf) + " IST";
  doc["date"] = String(dateBuf);
  doc["temperature"] = temp;
  doc["feels_like"] = feelsLike;
  doc["humidity"] = humidity;
  doc["pressure"] = pressure;
  doc["wind_speed"] = windSpeed;
  doc["wind_dir"] = windDirection;
  doc["cloud_cover"] = cloudCover;
  doc["uv_index"] = uvIndex;
  doc["condition"] = conditionText;
  doc["aqi"] = usAqi;
  doc["aqi_status"] = aqiStatus;
  doc["pm2_5"] = pm2_5;
  doc["pm10"] = pm10;

  String output;
  serializeJson(doc, output);
  client.publish(mqtt_topic, output.c_str());
}

// 5. Update OLED Screen
void updateOLED() {
  char timeBuf[12], dateBuf[12];
  getFormattedTime(timeBuf, dateBuf);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Line 1: IST Date & Live Time
  display.setCursor(0, 0);
  display.printf("%s %s", dateBuf, timeBuf);
  display.drawLine(0, 9, 128, 9, SSD1306_WHITE);

  // Line 2: Location & Weather
  display.setCursor(0, 12);
  display.printf("%s: %s", city.c_str(), conditionText.c_str());

  // Line 3: Temperature & Humidity
  display.setCursor(0, 24);
  display.printf("T:%.1fC(%.1f) H:%d%%", temp, feelsLike, humidity);

  // Line 4: Air Quality & UV
  display.setCursor(0, 36);
  display.printf("AQI:%d (%s) UV:%.1f", usAqi, aqiStatus.c_str(), uvIndex);

  // Line 5: Particulate Matter PM2.5 / PM10
  display.setCursor(0, 48);
  display.printf("PM2.5:%.1f PM10:%.0f", pm2_5, pm10);

  // Line 6: Barometer & Wind
  display.setCursor(0, 57);
  display.printf("P:%.0f W:%.1fkm/h", pressure, windSpeed);

  display.display();
}

void reconnectMqtt() {
  if (!client.connected()) {
    String clientId = "ESP8266-Weather-" + String(random(0xffff), HEX);
    client.connect(clientId.c_str());
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin(D2, D1);

  display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 10);
  display.println("Connecting WiFi...");
  display.display();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  // Initialize NTP explicitly for Indian Standard Time (IST)
  configTime(IST_OFFSET_SEC, DST_OFFSET_SEC, "pool.ntp.org", "time.google.com");

  display.clearDisplay();
  display.setCursor(0, 10);
  display.println("Detecting Location...");
  display.display();

  fetchLocation();
  fetchWeatherData();
  updateOLED();

  client.setServer(mqtt_server, mqtt_port);
}

void loop() {
  if (!client.connected()) {
    reconnectMqtt();
  }
  client.loop();

  // Update clock on OLED every second in IST
  static unsigned long lastClockUpdate = 0;
  if (millis() - lastClockUpdate >= 1000) {
    lastClockUpdate = millis();
    updateOLED();
  }

  // Refresh Weather & AQI APIs every 30 seconds
  unsigned long currentMillis = millis();
  if (currentMillis - lastWeatherFetch >= weatherInterval) {
    lastWeatherFetch = currentMillis;
    fetchWeatherData();
    publishTelemetry();
  }
}