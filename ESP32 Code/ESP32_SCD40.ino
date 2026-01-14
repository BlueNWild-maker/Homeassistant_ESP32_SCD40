#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiManager.h> 
#include <PubSubClient.h>
#include <SensirionI2cScd4x.h>
#include <Preferences.h> 

// --- Pins ---
#define RESET_PIN 0      
#define STATUS_LED 2     

// --- Variablen für Portal ---
char device_name[30] = "SCD40_Sensor"; 
char mqtt_server[40] = "192.168.0.207";
char mqtt_user[30]   = "";
char mqtt_pass[30]   = "";
char temp_offset_str[10] = "0"; 

WiFiClient espClient;
PubSubClient client(espClient);
SensirionI2cScd4x scd4x;

// Diese Variable enthält Zeit und Datum der Kompilierung
const char* build_time = __DATE__ " " __TIME__;

String getChipID() {
  return String((uint32_t)ESP.getEfuseMac(), HEX);
}

void sendDiscovery() {
  String id = getChipID();
  String baseTopic = "homeassistant/sensor/" + id;
  String devName = String(device_name); 
  String deviceJson = ",\"dev\":{\"ids\":[\"" + id + "\"],\"name\":\"" + devName + "\",\"mf\":\"Sensirion\",\"mdl\":\"SCD40 ESP32\"}";

  client.publish((baseTopic + "_co2/config").c_str(), ("{\"name\":\"CO2\",\"stat_t\":\"" + baseTopic + "/state\",\"unit_of_meas\":\"ppm\",\"val_tpl\":\"{{value_json.co2}}\",\"dev_cla\":\"carbon_dioxide\",\"uniq_id\":\"" + id + "_co2\"" + deviceJson + "}").c_str(), true);
  client.publish((baseTopic + "_temp/config").c_str(), ("{\"name\":\"Temperatur\",\"stat_t\":\"" + baseTopic + "/state\",\"unit_of_meas\":\"°C\",\"val_tpl\":\"{{value_json.temp}}\",\"dev_cla\":\"temperature\",\"uniq_id\":\"" + id + "_temp\"" + deviceJson + "}").c_str(), true);
  client.publish((baseTopic + "_hum/config").c_str(), ("{\"name\":\"Luftfeuchtigkeit\",\"stat_t\":\"" + baseTopic + "/state\",\"unit_of_meas\":\"%\",\"val_tpl\":\"{{value_json.hum}}\",\"dev_cla\":\"humidity\",\"uniq_id\":\"" + id + "_hum\"" + deviceJson + "}").c_str(), true);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  pinMode(STATUS_LED, OUTPUT);
  Wire.begin(21, 22); 

  // --- AUTOMATISCHER RESET BEI JEDEM FLASH ---
  Preferences prefs;
  prefs.begin("setup", false);
  
  String last_build = prefs.getString("last_build", "");
  
  if (last_build != String(build_time)) {
    Serial.println("--- NEUER FLASH VORGANG ERKANNT ---");
    WiFiManager wm;
    wm.resetSettings(); // Löscht WLAN & MQTT Daten für das Portal
    prefs.putString("last_build", build_time);
    Serial.println("Einstellungen für Portal-Neustart zurückgesetzt.");
  } else {
    Serial.println("--- NORMALER REBOOT (Daten bleiben erhalten) ---");
  }

  // Gespeicherte Werte laden
  prefs.getString("dev_name", device_name, 30);
  prefs.getString("offset", temp_offset_str, 10);
  prefs.end();

  WiFiManager wm;
  WiFiManagerParameter custom_device_name("name", "Name in HA", device_name, 30);
  WiFiManagerParameter custom_mqtt_server("server", "MQTT IP", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_user("user", "User", mqtt_user, 30);
  WiFiManagerParameter custom_mqtt_pass("pass", "Pass", mqtt_pass, 30);
  WiFiManagerParameter custom_temp_offset("offset", "Temperatur Offset", temp_offset_str, 10);
  
  wm.addParameter(&custom_device_name);
  wm.addParameter(&custom_mqtt_server);
  wm.addParameter(&custom_mqtt_user);
  wm.addParameter(&custom_mqtt_pass);
  wm.addParameter(&custom_temp_offset);

  // Portal starten
  if (!wm.autoConnect("ESP32_SCD40_Portal")) {
    ESP.restart();
  }

  // Werte übernehmen und sichern
  strcpy(device_name, custom_device_name.getValue());
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_user, custom_mqtt_user.getValue());
  strcpy(mqtt_pass, custom_mqtt_pass.getValue());
  strcpy(temp_offset_str, custom_temp_offset.getValue());

  prefs.begin("setup", false);
  prefs.putString("dev_name", device_name);
  prefs.putString("offset", temp_offset_str);
  prefs.end();
  
  IPAddress mqttIP;
  if (mqttIP.fromString(mqtt_server)) client.setServer(mqttIP, 1883);
  else client.setServer(mqtt_server, 1883);

  scd4x.begin(Wire, 0x62); 
  scd4x.stopPeriodicMeasurement(); 
  
  float offsetVal = atof(temp_offset_str);
  if (offsetVal != 0.0) {
    scd4x.setTemperatureOffset(offsetVal);
  }
  
  scd4x.persistSettings();
  scd4x.startLowPowerPeriodicMeasurement(); 
  client.setBufferSize(1024);
}

void reconnect() {
  while (!client.connected()) {
    String clientId = "ESP32-" + getChipID();
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      sendDiscovery();
    } else {
      delay(5000);
    }
  }
}

void loop() {
  // Manueller Reset Button (5 Sek halten)
  if (digitalRead(0) == LOW) {
    unsigned long s = millis();
    while(digitalRead(0) == LOW) {
      if (millis() - s > 5000) { WiFiManager wm; wm.resetSettings(); ESP.restart(); }
    }
  }

  if (!client.connected()) reconnect();
  client.loop();

  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 30000) { 
    lastCheck = millis();
    uint16_t co2; float temp, hum;
    if (scd4x.readMeasurement(co2, temp, hum) == 0 && co2 != 0) {
      digitalWrite(STATUS_LED, HIGH);
      String stateTopic = "homeassistant/sensor/" + getChipID() + "/state";
      String payload = "{\"co2\":" + String(co2) + ",\"temp\":" + String(temp, 1) + ",\"hum\":" + String(hum, 1) + "}";
      client.publish(stateTopic.c_str(), payload.c_str());
      delay(100);
      digitalWrite(STATUS_LED, LOW);
    }
  }
}