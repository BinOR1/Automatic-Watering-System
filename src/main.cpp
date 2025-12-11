#include <Arduino.h>
#include <Wire.h>
#include "secrets/wifi.h"
#include "wifi_connect.h"
#include <WiFiClientSecure.h>
#include "ca_cert_emqx.h"
#include "secrets/mqtt.h"
#include <PubSubClient.h>
#include "MQTT.h"
#include <DHT.h>
#include <Ticker.h>
#include <Adafruit_Sensor.h>

// Soil Thresholds
#define SOIL_DRY_THRESHOLD 30 // Lower than 30% = dry, turn on pump
#define SOIL_WET_THRESHOLD 60 // Higher than 60% = wet enough, turn off pump
#define SOIL_WET_VALUE 800   // ADC value when sensor is wet
#define SOIL_DRY_VALUE 3500  // ADC value when sensor is dry
#define NUM_SOIL_SENSORS 3

#define PUMP_MAX_DURATION 10000 // Maximum pump run time
#define PUMP_MIN_OFF_TIME 6000 // Minimum time between watering

// Pin of soil moisture sensors
const int PIN_SOIL_1 = 32;
const int PIN_SOIL_2 = 33;
const int PIN_SOIL_3 = 34;

// Gas (MQ-2)
const int PIN_MQ2 = 35; // Gas sensor analog pin

// Pin of DHT, Relay, Pump status LEDs
const int DHT11PIN = 18; // DHT11 data pin
const int PIN_RELAY_PUMP = 26;
const int PIN_LED_PUMP_RED = 22;   // Red LED : pump OFF
const int PIN_LED_PUMP_GREEN = 4; // Green LED : pump ON

int soilMoistureValues[NUM_SOIL_SENSORS] = {0, 0, 0};

// Flags for non-blocking operation
volatile bool shouldReadSensors = false;
volatile bool shouldCheckPumpSafety = false;
unsigned long lastSensorRead = 0;
unsigned long lastPumpCheck = 0;
const unsigned long SENSOR_INTERVAL = 1000;     // 1 second
const unsigned long PUMP_CHECK_INTERVAL = 1000; // 1 second

namespace
{
    const char *ssid = WiFiSecrets::ssid;
    const char *password = WiFiSecrets::pass;
    const char *client_id = (String("esp32-client") + WiFi.macAddress()).c_str();

    DHT dht(DHT11PIN, DHT11);
    WiFiClientSecure tlsClient;
    PubSubClient mqttClient(tlsClient);

    Ticker sensorTicker;
    Ticker pumpCheckTicker;
    const char *temperature_topic = "plant/sensor/temperature";
    const char *humidity_topic = "plant/sensor/humidity";
    const char *soil_1_topic = "plant/sensor/soil1";
    const char *soil_2_topic = "plant/sensor/soil2";
    const char *soil_3_topic = "plant/sensor/soil3";
    const char *soil_avg_topic = "plant/sensor/soil_avg";
    const char *gas_topic = "plant/sensor/gas";
    const char *light_topic = "plant/sensor/light";
    const char *pump_status_topic = "plant/pump/status";
    const char *pump_control_topic = "plant/pump/control";

    // Pump state variables
    bool isPumpOn = false;
    bool autoMode = false; 
    unsigned long pumpStartTime = 0;
    unsigned long lastPumpOffTime = 0;
    int wateringCount = 0;

    // Sensor pins array
    int soilPins[NUM_SOIL_SENSORS] = {PIN_SOIL_1, PIN_SOIL_2, PIN_SOIL_3};

}

// Ticker callback functions
void sensorTickerHandler()
{
    shouldReadSensors = true;
}

void pumpCheckTickerHandler()
{
    shouldCheckPumpSafety = true;
}

// SOIL MOISTURE FUNCTIONS
int mapSoilMoisture(int rawValue)
{
    int moisture = map(rawValue, SOIL_DRY_VALUE, SOIL_WET_VALUE, 0, 100);
    return constrain(moisture, 0, 100);
}

float readAverageSoilMoisture()
{
    int totalMoisture = 0;
    int validSensors = 0;

    for (int i = 0; i < NUM_SOIL_SENSORS; i++)
    {
        int rawValue = analogRead(soilPins[i]);
        int moisture = mapSoilMoisture(rawValue);

        Serial.printf("Soil %d: %d%% (raw: %d)\n", i + 1, moisture, rawValue);

        soilMoistureValues[i] = moisture;

        if (moisture >= 0)
        {
            totalMoisture += moisture;
            validSensors++;
        }
    }

    if (validSensors > 0)
    {
        return (float)totalMoisture / validSensors;
    }
    return 0;
}

// PUMP CONTROL FUNCTIONS
bool canStartPump()
{
    // Check minimum off time between watering
    if (lastPumpOffTime > 0)
    {
        unsigned long timeSinceLastOff = millis() - lastPumpOffTime;
        if (timeSinceLastOff < PUMP_MIN_OFF_TIME)
        {
            Serial.printf("Pump cooling down: %lu/%lu seconds\n",
                          timeSinceLastOff / 1000, PUMP_MIN_OFF_TIME / 1000);
            return false;
        }
    }
    return true;
}

void turnPumpOn()
{
    if (!isPumpOn && canStartPump())
    {
        Serial.println(" [PUMP] Turning ON...");
        
        noInterrupts();
        digitalWrite(PIN_RELAY_PUMP, HIGH);

        digitalWrite(PIN_LED_PUMP_GREEN, HIGH);
        digitalWrite(PIN_LED_PUMP_RED, LOW);
        interrupts();
        
        isPumpOn = true;
        pumpStartTime = millis();
        wateringCount++;

        Serial.println(" [PUMP] Pump turned ON");

        mqttClient.publish(pump_status_topic, "ON", true);
    }
}

void turnPumpOff()
{
    if (isPumpOn)
    {
        Serial.println(" [PUMP] Turning OFF...");
        
        noInterrupts();
        digitalWrite(PIN_RELAY_PUMP, LOW);

        digitalWrite(PIN_LED_PUMP_GREEN, LOW);
        digitalWrite(PIN_LED_PUMP_RED, HIGH);
        interrupts();

        unsigned long duration = millis() - pumpStartTime;
        isPumpOn = false;
        lastPumpOffTime = millis();

        Serial.printf(" [PUMP] Pump turned OFF (ran for %lu seconds)\n", duration / 1000);

        mqttClient.publish(pump_status_topic, "OFF", true);
    }
}

void checkPumpSafety()
{
    if (isPumpOn)
    {
        unsigned long runTime = millis() - pumpStartTime;
        if (runTime > PUMP_MAX_DURATION)
        {
            Serial.println(" SAFETY: Pump ran too long, forcing shutdown!");
            turnPumpOff();
        }
    }
}

void autoControlPump(float soilMoisture)
{
    if (!autoMode)
        return;

    if (!isPumpOn)
    {
        if (soilMoisture < SOIL_DRY_THRESHOLD)
        {
            Serial.printf(" Soil too dry (%.1f%% < %d%%), starting pump\n",
                          soilMoisture, SOIL_DRY_THRESHOLD);
            turnPumpOn();
        }
    }
    else
    {
        if (soilMoisture >= SOIL_WET_THRESHOLD)
        {
            Serial.printf(" Soil moisture sufficient (%.1f%% >= %d%%), stopping pump\n",
                          soilMoisture, SOIL_WET_THRESHOLD);
            turnPumpOff();
        }
    }
}

// SENSOR READ & PUBLISH
void sensorReadPublish()
{
    unsigned long currentTime = millis();
    if (isPumpOn && (currentTime - pumpStartTime < 1000))
    {
        Serial.println(" [SENSOR] Skipping read - pump just turned ON");
        return;
    }
    if (!isPumpOn && lastPumpOffTime > 0 && (currentTime - lastPumpOffTime < 1000))
    {
        Serial.println(" [SENSOR] Skipping read - pump just turned OFF");
        return;
    }

    //read dht sensor
    float temperature = dht.readTemperature();
    float humidity = dht.readHumidity();

    if (isnan(temperature) || isnan(humidity) ||
        temperature <= -40 || temperature >= 80 ||
        humidity <= 0 || humidity >= 100)
    {
        Serial.println(" [DHT] Invalid reading, skip this cycle");
        return;
    }
    int gasRaw = analogRead(PIN_MQ2);
    int gasPercent = map(constrain(gasRaw, 0, 4095), 0, 4095, 0, 100);

    // Read soil moisture
    float avgSoilMoisture = readAverageSoilMoisture();

    // Print to Serial
    Serial.println("\n=== Sensor Data ===");
    Serial.printf("Temperature: %.1f°C\n", temperature);
    Serial.printf("Humidity: %.1f%%\n", humidity);
    Serial.printf("Gas: %d (raw), %d%%\n", gasRaw, gasPercent);
    Serial.printf("Average Soil: %.1f%%\n", avgSoilMoisture);
    Serial.printf("Pump: %s (Mode: %s)\n",
                  isPumpOn ? "ON" : "OFF",
                  autoMode ? "AUTO" : "MANUAL");
    Serial.println("==================\n");

    // Publish each sensor value to separate topics
    if (temperature > 0 || humidity > 0)
    {
        mqttClient.publish(temperature_topic, String(temperature, 1).c_str(), false);
        yield();
        delay(50);
        mqttClient.publish(humidity_topic, String(humidity, 1).c_str(), false);
        yield();
        delay(50);
    }
    mqttClient.publish(gas_topic, String(gasPercent).c_str(), false);
    yield();
    delay(50);
    mqttClient.publish(soil_1_topic, String(soilMoistureValues[0]).c_str(), false);
    yield();
    delay(50);
    mqttClient.publish(soil_2_topic, String(soilMoistureValues[1]).c_str(), false);
    yield();
    delay(50);
    mqttClient.publish(soil_3_topic, String(soilMoistureValues[2]).c_str(), false);
    yield();
    delay(50);
    mqttClient.publish(soil_avg_topic, String(avgSoilMoisture, 1).c_str(), false);
    yield();
    mqttClient.publish(gas_topic, String(gasPercent).c_str(), false);
    yield();

    Serial.println(" [MQTT] Published sensor data");

    // Auto control pump based on soil moisture
    autoControlPump(avgSoilMoisture);
}


void mqttCallback(char *topic, uint8_t *payload, unsigned int length)
{
    // Convert payload to string
    String message = "";
    for (unsigned int i = 0; i < length; i++)
    {
        message += (char)payload[i];
    }

    Serial.printf(" MQTT received [%s]: %s\n", topic, message.c_str());

    // Handle pump control commands
    if (strcmp(topic, pump_control_topic) == 0)
    {
        message.toLowerCase();

        if (message == "on" || message == "1")
        {
            autoMode = false;
            turnPumpOn();
        }
        else if (message == "off" || message == "0")
        {
            autoMode = false;
            turnPumpOff();
        }
        else if (message == "auto")
        {
            autoMode = true;
            Serial.println(" Switched to AUTO mode");
        }
        else if (message == "manual")
        {
            autoMode = false;
            Serial.println(" Switched to MANUAL mode");
        }
        else if (message == "toggle")
        {
            if (isPumpOn)
            {
                turnPumpOff();
            }
            else
            {
                turnPumpOn();
            }
        }
    }
}

void setup()
{
    Serial.begin(115200);
    delay(10);

    // Initialize pins
    pinMode(PIN_RELAY_PUMP, OUTPUT);
    pinMode(PIN_LED_PUMP_RED, OUTPUT);
    pinMode(PIN_LED_PUMP_GREEN, OUTPUT);

    digitalWrite(PIN_RELAY_PUMP, LOW);
    digitalWrite(PIN_LED_PUMP_RED, HIGH);
    digitalWrite(PIN_LED_PUMP_GREEN, LOW);

    isPumpOn = false;
    autoMode = false;

    // Initialize ADC for analog sensors (soil, gas, LDR)
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    // Initialize DHT sensor
    dht.begin();

    setup_wifi(ssid, password);
    tlsClient.setCACert(ca_cert);

    mqttClient.setCallback(mqttCallback);
    mqttClient.setServer(EMQX::broker, EMQX::port);

    sensorTicker.attach_ms(SENSOR_INTERVAL, sensorTickerHandler);
    pumpCheckTicker.attach_ms(PUMP_CHECK_INTERVAL, pumpCheckTickerHandler);
}

void loop()
{
    MQTT::reconnect(mqttClient, client_id, EMQX::username, EMQX::password, pump_control_topic);
    mqttClient.loop();

    if (shouldReadSensors)
    {
        shouldReadSensors = false;
        sensorReadPublish();
    }

    if (shouldCheckPumpSafety)
    {
        shouldCheckPumpSafety = false;
        checkPumpSafety();
    }

    delay(10);
    yield(); 
}