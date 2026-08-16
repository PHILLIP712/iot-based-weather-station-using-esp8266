#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>

// Wi-Fi Credentials
const char* ssid = "Infinix";
const char* password = "1234567890";

// Public MQTT Broker Configuration
const char* mqtt_server = "broker.emqx.io";
const int mqtt_port = 1883;
const char* mqtt_topic = "arpan_weather_station/state";

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

// Current Meteorological Telemetry
float temp = 0.0, feelsLike = 0.0, pressure = 0.0, windSpeed = 0.0, uvIndex = 0.0;
int humidity = 0, cloudCover = 0, weatherCode = 0;
String conditionText = "Updating...";

// Air Quality Telemetry
int usAqi = 0;
float pm2_5 = 0.0, pm10 = 0.0;
String aqiStatus = "Good";

// Timers
unsigned long lastWeatherFetch = 0;
const unsigned long weatherInterval = 30000;

unsigned long lastMqttPublish = 0;
const unsigned long mqttInterval = 1000;

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

  String weatherUrl = "https://api.open-meteo.com/v1/forecast?latitude=" + String(latitude, 4) +
                      "&longitude=" + String(longitude, 4) +
                      "&current=temperature_2m,relative_humidity_2m,apparent_temperature,surface_pressure,wind_speed_10m,cloud_cover,uv_index,weather_code";

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
      cloudCover = current["cloud_cover"];
      uvIndex = current["uv_index"];
      weatherCode = current["weather_code"];
      conditionText = getWeatherDescription(weatherCode);
    }
    https.end();
  }

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
  strftime(timeBuffer, 10, "%H:%M:%S", timeinfo);
  strftime(dateBuffer, 12, "%d-%b-%Y", timeinfo);
}

void publishTelemetry() {
  if (!client.connected()) return;

  char timeBuf[10], dateBuf[12];
  getFormattedTime(timeBuf, dateBuf);

  DynamicJsonDocument doc(1024);
  doc["city"] = city;
  doc["country"] = country;
  doc["lat"] = latitude;
  doc["lon"] = longitude;
  doc["time"] = String(timeBuf) + " IST";
  doc["date"] = String(dateBuf);
  doc["temperature"] = temp;
  doc["feels_like"] = feelsLike;
  doc["humidity"] = humidity;
  doc["pressure"] = pressure;
  doc["wind_speed"] = windSpeed;
  doc["cloud_cover"] = cloudCover;
  doc["uv_index"] = uvIndex;
  doc["condition"] = conditionText;
  doc["aqi"] = usAqi;
  doc["aqi_status"] = aqiStatus;
  doc["pm2_5"] = pm2_5;
  doc["pm10"] = pm10;

  String output;
  serializeJson(doc, output);
  client.publish(mqtt_topic, output.c_str(), true);
}

void reconnectMqtt() {
  if (!client.connected()) {
    String clientId = "ESP8266-Weather-" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      publishTelemetry();
    }
  }
}

void setup() {
  Serial.begin(115200);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  configTime(IST_OFFSET_SEC, DST_OFFSET_SEC, "pool.ntp.org", "time.google.com");

  fetchLocation();
  fetchWeatherData();

  client.setServer(mqtt_server, mqtt_port);
  client.setBufferSize(1024);
  reconnectMqtt();
}

void loop() {
  if (!client.connected()) {
    reconnectMqtt();
  }
  client.loop();

  unsigned long currentMillis = millis();

  if (currentMillis - lastMqttPublish >= mqttInterval) {
    lastMqttPublish = currentMillis;
    publishTelemetry();
  }

  if (currentMillis - lastWeatherFetch >= weatherInterval) {
    lastWeatherFetch = currentMillis;
    fetchWeatherData();
  }
}