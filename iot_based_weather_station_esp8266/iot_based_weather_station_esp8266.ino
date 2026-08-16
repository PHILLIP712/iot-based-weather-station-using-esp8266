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

// IST Timezone: UTC +5:30 (19800 seconds, 0 DST)
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
String conditionText = "Updating...";

// Air Quality Telemetry Variables
int usAqi = 0;
float pm2_5 = 0.0;
float pm10 = 0.0;
String aqiStatus = "Good";

// Timers & Page Management
int currentPage = 0;
const int totalPages = 3;
unsigned long lastPageSwitch = 0;
const unsigned long pageInterval = 4000;    // Switch page every 4 seconds

unsigned long lastWeatherFetch = 0;
const unsigned long weatherInterval = 30000; // API fetch every 30 seconds

unsigned long lastMqttPublish = 0;
const unsigned long mqttInterval = 5000;     // Publish MQTT every 5 seconds

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

void getFormattedTime(char* timeBuffer, char* dateBuffer) {
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);
  strftime(timeBuffer, 12, "%I:%M:%S%p", timeinfo);
  strftime(dateBuffer, 12, "%d-%b-%Y", timeinfo);
}

void publishTelemetry() {
  if (!client.connected()) return;

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

// Draw bottom navigation dots [ • ○ ○ ]
void drawPageDots(int active) {
  int startX = 54;
  int y = 60;
  for (int i = 0; i < totalPages; i++) {
    if (i == active) {
      display.fillCircle(startX + (i * 10), y, 2, SSD1306_WHITE);
    } else {
      display.drawCircle(startX + (i * 10), y, 2, SSD1306_WHITE);
    }
  }
}

// Render Paginated OLED Screens
void renderOLED() {
  char timeBuf[12], dateBuf[12];
  getFormattedTime(timeBuf, dateBuf);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  if (currentPage == 0) {
    // === PAGE 1: CLOCK & WEATHER ===
    display.setCursor(0, 0);
    display.printf("%s %s", dateBuf, timeBuf);
    display.drawLine(0, 9, 128, 9, SSD1306_WHITE);

    display.setCursor(0, 13);
    display.printf("Status: %s", conditionText.c_str());

    display.setCursor(0, 26);
    display.printf("Temp:   %.1f C", temp);

    display.setCursor(0, 37);
    display.printf("Feels:  %.1f C", feelsLike);

    display.setCursor(0, 48);
    display.printf("Humid:  %d %%", humidity);

  } else if (currentPage == 1) {
    // === PAGE 2: AIR QUALITY (AQI) ===
    display.setCursor(0, 0);
    display.printf("AQI: %s, %s", city.c_str(), country.c_str());
    display.drawLine(0, 9, 128, 9, SSD1306_WHITE);

    display.setCursor(0, 14);
    display.printf("US AQI: %d", usAqi);

    display.setCursor(0, 26);
    display.printf("State:  %s", aqiStatus.c_str());

    display.setCursor(0, 38);
    display.printf("PM2.5:  %.1f ug/m3", pm2_5);

    display.setCursor(0, 49);
    display.printf("PM10:   %.1f ug/m3", pm10);

  } else if (currentPage == 2) {
    // === PAGE 3: ATMOSPHERE & SOLAR ===
    display.setCursor(0, 0);
    display.print("Atmosphere & Wind");
    display.drawLine(0, 9, 128, 9, SSD1306_WHITE);

    display.setCursor(0, 14);
    display.printf("Press:  %.0f hPa", pressure);

    display.setCursor(0, 26);
    display.printf("Wind:   %.1f km/h", windSpeed);

    display.setCursor(0, 38);
    display.printf("Dir:    %d deg", windDirection);

    display.setCursor(0, 49);
    display.printf("UV: %.1f | Cloud: %d%%", uvIndex, cloudCover);
  }

  // Draw Page Indicator
  drawPageDots(currentPage);
  display.display();
}

void reconnectMqtt() {
  if (!client.connected()) {
    String clientId = "ESP8266-Weather-" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      publishTelemetry(); // Immediately send data upon connection
    }
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

  configTime(IST_OFFSET_SEC, DST_OFFSET_SEC, "pool.ntp.org", "time.google.com");

  display.clearDisplay();
  display.setCursor(0, 10);
  display.println("Detecting Location...");
  display.display();

  fetchLocation();
  fetchWeatherData();

  client.setServer(mqtt_server, mqtt_port);
  reconnectMqtt();
  renderOLED();
}

void loop() {
  if (!client.connected()) {
    reconnectMqtt();
  }
  client.loop();

  unsigned long currentMillis = millis();

  // 1. Page Cycling every 4 seconds
  if (currentMillis - lastPageSwitch >= pageInterval) {
    lastPageSwitch = currentMillis;
    currentPage = (currentPage + 1) % totalPages;
  }

  // 2. Render OLED (clock updates every cycle smoothly)
  static unsigned long lastOledDraw = 0;
  if (currentMillis - lastOledDraw >= 500) {
    lastOledDraw = currentMillis;
    renderOLED();
  }

  // 3. Publish to MQTT every 5 seconds (keeps GitHub dashboard updated)
  if (currentMillis - lastMqttPublish >= mqttInterval) {
    lastMqttPublish = currentMillis;
    publishTelemetry();
  }

  // 4. Fetch fresh Open-Meteo API data every 30 seconds
  if (currentMillis - lastWeatherFetch >= weatherInterval) {
    lastWeatherFetch = currentMillis;
    fetchWeatherData();
  }
}