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

// Meteorological Telemetry
float temp = 0.0, feelsLike = 0.0, pressure = 0.0, windSpeed = 0.0, uvIndex = 0.0;
int humidity = 0, windDirection = 0, cloudCover = 0, weatherCode = 0;
String conditionText = "Updating...";

// Air Quality Telemetry
int usAqi = 0;
float pm2_5 = 0.0, pm10 = 0.0;
String aqiStatus = "Good";

// Solar Ephemeris
String sunriseTime = "--:--", sunsetTime = "--:--";
float daylightHours = 0.0;

// 3-Day Daily Forecast
struct DayForecast {
  String date;
  float maxTemp;
  float minTemp;
  int pop;
  int code;
  String condition;
};
DayForecast forecast[3];

// 12-Hour Timeline Structure
struct HourForecast {
  String time;
  float temp;
  int pop;
  String condition;
};
HourForecast hourly[12];

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
                      "&current=temperature_2m,relative_humidity_2m,apparent_temperature,surface_pressure,wind_speed_10m,wind_direction_10m,cloud_cover,uv_index,weather_code" +
                      "&hourly=temperature_2m,precipitation_probability,weather_code" +
                      "&daily=sunrise,sunset,daylight_duration,temperature_2m_max,temperature_2m_min,precipitation_probability_max,weather_code&timezone=Asia%2FKolkata&forecast_days=3";

  if (https.begin(clientSecure, weatherUrl)) {
    if (https.GET() == HTTP_CODE_OK) {
      DynamicJsonDocument doc(6144);
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

      String rawRise = doc["daily"]["sunrise"][0].as<String>();
      String rawSet = doc["daily"]["sunset"][0].as<String>();
      if (rawRise.length() >= 16) sunriseTime = rawRise.substring(11, 16);
      if (rawSet.length() >= 16) sunsetTime = rawSet.substring(11, 16);
      daylightHours = doc["daily"]["daylight_duration"][0].as<float>() / 3600.0;

      // 3-Day Forecast
      for (int i = 0; i < 3; i++) {
        forecast[i].date = doc["daily"]["time"][i].as<String>();
        forecast[i].maxTemp = doc["daily"]["temperature_2m_max"][i];
        forecast[i].minTemp = doc["daily"]["temperature_2m_min"][i];
        forecast[i].pop = doc["daily"]["precipitation_probability_max"][i] | 0;
        forecast[i].code = doc["daily"]["weather_code"][i];
        forecast[i].condition = getWeatherDescription(forecast[i].code);
      }

      // Next 12 Hourly Forecast Frames
      time_t now = time(nullptr);
      struct tm* timeinfo = localtime(&now);
      int currentHour = timeinfo->tm_hour;

      for (int i = 0; i < 12; i++) {
        int idx = currentHour + i;
        if (idx < 72) {
          String rawTime = doc["hourly"]["time"][idx].as<String>();
          hourly[i].time = (rawTime.length() >= 16) ? rawTime.substring(11, 16) : String(idx % 24) + ":00";
          hourly[i].temp = doc["hourly"]["temperature_2m"][idx];
          hourly[i].pop = doc["hourly"]["precipitation_probability"][idx] | 0;
          hourly[i].condition = getWeatherDescription(doc["hourly"]["weather_code"][idx]);
        }
      }
    }
    https.end();
  }

  // Air Quality API
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

  DynamicJsonDocument doc(3072);
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
  doc["sunrise"] = sunriseTime;
  doc["sunset"] = sunsetTime;
  doc["daylight"] = daylightHours;

  // Daily Forecast
  JsonArray fcArray = doc.createNestedArray("forecast");
  for (int i = 0; i < 3; i++) {
    JsonObject fc = fcArray.createNestedObject();
    fc["date"] = forecast[i].date;
    fc["max"] = forecast[i].maxTemp;
    fc["min"] = forecast[i].minTemp;
    fc["pop"] = forecast[i].pop;
    fc["cond"] = forecast[i].condition;
  }

  // Hourly Timeline
  JsonArray hrArray = doc.createNestedArray("hourly");
  for (int i = 0; i < 12; i++) {
    JsonObject hr = hrArray.createNestedObject();
    hr["time"] = hourly[i].time;
    hr["temp"] = hourly[i].temp;
    hr["pop"] = hourly[i].pop;
    hr["cond"] = hourly[i].condition;
  }

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
  client.setBufferSize(3072);
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