#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiManager.h> 
#include <PubSubClient.h>
#include <SensirionI2cScd4x.h>
#include <Preferences.h> 

// --- Configuration ---
#define RESET_PIN 0      
#define STATUS_LED 2     

char device_name[30] = "SCD40_Sensor"; 
char mqtt_server[40] = "";
char mqtt_user[30]   = "";
char mqtt_pass[30]   = "";
char temp_offset_str[10] = "0"; 

WiFiClient espClient;
PubSubClient client(espClient);
SensirionI2cScd4x scd4x;

// Build timestamp for automatic flash detection
const char* build_time = __DATE__ " " __TIME__;
int error_count = 0; // I2C Watchdog counter

String getChipID() { 
  return String((uint32_t)ESP.getEfuseMac(), HEX); 
}

void sendDiscovery() {
  String id = getChipID();
  String baseTopic = "homeassistant/sensor/" + id;
  String devName = String(device_name); 
  String deviceJson = ",\"dev\":{\"ids\":[\"" + id + "\"],\"name\":\"" + devName + "\",\"mf\":\"Sensirion\",\"mdl\":\"SCD40 ESP32\"}";

  // Availability topic (LWT)
  String availTopic = baseTopic + "/status";

  // MQTT Discovery Payloads
  client.publish((baseTopic + "_co2/config").c_str(), ("{\"name\":\"CO2\",\"stat_t\":\"" + baseTopic + "/state\",\"unit_of_meas\":\"ppm\",\"val_tpl\":\"{{value_json.co2}}\",\"dev_cla\":\"carbon_dioxide\",\"uniq_id\":\"" + id + "_co2\",\"avty_t\":\"" + availTopic + "\"" + deviceJson + "}").c_str(), true);
  client.publish((baseTopic + "_temp/config").c_str(), ("{\"name\":\"Temperature\",\"stat_t\":\"" + baseTopic + "/state\",\"unit_of_meas\":\"°C\",\"val_tpl\":\"{{value_json.temp}}\",\"dev_cla\":\"temperature\",\"uniq_id\":\"" + id + "_temp\",\"avty_t\":\"" + availTopic + "\"" + deviceJson + "}").c_str(), true);
  client.publish((baseTopic + "_hum/config").c_str(), ("{\"name\":\"Humidity\",\"stat_t\":\"" + baseTopic + "/state\",\"unit_of_meas\":\"%\",\"val_tpl\":\"{{value_json.hum}}\",\"dev_cla\":\"humidity\",\"uniq_id\":\"" + id + "_hum\",\"avty_t\":\"" + availTopic + "\"" + deviceJson + "}").c_str(), true);
  
  // Set status to online
  client.publish(availTopic.c_str(), "online", true);
  Serial.println("HA Discovery sent.");
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n--- BOOTING SYSTEM ---");
  
  pinMode(STATUS_LED, OUTPUT);
  Wire.begin(21, 22); 

  // Flash-Reset Logic: Compare current build time with stored build time
  Preferences prefs;
  prefs.begin("setup", false);
  if (prefs.getString("last_build", "") != String(build_time)) {
    Serial.println("New build detected! Resetting settings for Config Portal...");
    WiFiManager wm;
    wm.resetSettings();
    prefs.putString("last_build", build_time);
  } else {
    Serial.println("Normal reboot. Loading saved settings.");
  }
  
  // Load stored parameters
  prefs.getString("dev_name", device_name, 30);
  prefs.getString("offset", temp_offset_str, 10);
  prefs.end();

  // Setup WiFiManager Portal
  WiFiManager wm;
  WiFiManagerParameter custom_device_name("name", "Device Name", device_name, 30);
  WiFiManagerParameter custom_mqtt_server("server", "MQTT Broker IP", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_user("user", "MQTT Username", mqtt_user, 30);
  WiFiManagerParameter custom_mqtt_pass("pass", "MQTT Password", mqtt_pass, 30);
  WiFiManagerParameter custom_temp_offset("offset", "Temp Offset (e.g. -4.1)", temp_offset_str, 10);
  
  wm.addParameter(&custom_device_name);
  wm.addParameter(&custom_mqtt_server);
  wm.addParameter(&custom_mqtt_user);
  wm.addParameter(&custom_mqtt_pass);
  wm.addParameter(&custom_temp_offset);

  if (!wm.autoConnect("ESP32_SCD40_Portal")) {
    Serial.println("Failed to connect. Restarting...");
    ESP.restart();
  }

  // Save values from portal to variables
  strcpy(device_name, custom_device_name.getValue());
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_user, custom_mqtt_user.getValue());
  strcpy(mqtt_pass, custom_mqtt_pass.getValue());
  strcpy(temp_offset_str, custom_temp_offset.getValue());

  // Persist values in flash
  prefs.begin("setup", false);
  prefs.putString("dev_name", device_name);
  prefs.putString("offset", temp_offset_str);
  prefs.end();
  
  // MQTT Server Setup
  IPAddress mqttIP;
  if (mqttIP.fromString(mqtt_server)) client.setServer(mqttIP, 1883);
  else client.setServer(mqtt_server, 1883);

  // SCD40 Initialisation
  Serial.println("Initializing SCD40...");
  scd4x.begin(Wire, 0x62); 
  scd4x.stopPeriodicMeasurement(); 
  
  float offsetVal = atof(temp_offset_str);
  if (offsetVal != 0.0) {
    Serial.print("Setting Temp Offset to: ");
    Serial.println(offsetVal);
    scd4x.setTemperatureOffset(offsetVal);
  }
  
  scd4x.persistSettings();
  scd4x.startLowPowerPeriodicMeasurement(); 
  client.setBufferSize(1024);
  Serial.println("Setup completed successfully!");
}

void reconnect() {
  String availTopic = "homeassistant/sensor/" + getChipID() + "/status";
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Last Will and Testament: Automatically reports "offline" if connection is lost
    if (client.connect(("ESP32-" + getChipID()).c_str(), mqtt_user, mqtt_pass, availTopic.c_str(), 0, true, "offline")) {
      Serial.println("connected!");
      sendDiscovery();
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

void loop() {
  // WiFi Auto-Reconnect logic
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection lost. Reconnecting...");
    WiFi.begin(); 
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) delay(500);
  }

  // Manual Reset Button (Hold for 5 seconds)
  if (digitalRead(0) == LOW) {
    unsigned long s = millis();
    while(digitalRead(0) == LOW) {
      if (millis() - s > 5000) { 
        Serial.println("Manual Factory Reset triggered.");
        WiFiManager wm; 
        wm.resetSettings(); 
        ESP.restart(); 
      }
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!client.connected()) reconnect();
    client.loop();
  }

  static unsigned long lastCheck = 0;
  static unsigned long lastForceSend = 0;
  static uint16_t last_co2 = 0;
  static float last_temp = 0.0, last_hum = 0.0;

  // Measurement Loop (Every 30 seconds)
  if (millis() - lastCheck > 30000) { 
    lastCheck = millis();
    uint16_t co2; float temp, hum;
    
    if (scd4x.readMeasurement(co2, temp, hum) == 0 && co2 != 0) {
      error_count = 0; // Reset I2C Watchdog
      
      // Determine if values changed significantly
      bool changeCO2 = abs((int)co2 - (int)last_co2) > 20;
      bool changeTemp = abs(temp - last_temp) > 0.2;
      bool changeHum = abs(hum - last_hum) > 1.0;
      bool forceUpdate = (millis() - lastForceSend > 300000); // Send heartbeat every 5 min

      if (changeCO2 || changeTemp || changeHum || forceUpdate) {
        last_co2 = co2; last_temp = temp; last_hum = hum;
        lastForceSend = millis();
        
        digitalWrite(STATUS_LED, HIGH);
        String payload = "{\"co2\":" + String(co2) + ",\"temp\":" + String(temp, 1) + ",\"hum\":" + String(hum, 1) + "}";
        client.publish(("homeassistant/sensor/" + getChipID() + "/state").c_str(), payload.c_str());
        Serial.println("Data sent: " + payload);
        delay(100);
        digitalWrite(STATUS_LED, LOW);
      } else {
        Serial.println("No significant changes detected.");
      }
    } else {
      error_count++;
      Serial.println("Sensor read error or data not ready.");
      if (error_count > 5) { // I2C Watchdog: Re-initialize Bus
        Serial.println("Persistent I2C error. Resetting I2C Bus...");
        Wire.begin(21, 22);
        scd4x.begin(Wire, 0x62);
        error_count = 0;
      }
    }
  }
}