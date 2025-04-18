// Read temperature and humidity data from an Arduino MKR1000 or MKR1010 device using a DHT11/DHT22 sensor.
// The data is then sent to Azure IoT Central for visualizing via MQTT
//
// https://github.com/firedog1024/mkr1000-iotc/blob/master/mkr10x0_iotc/mkr10x0_iotc.ino

#include <stdarg.h>
#include <time.h>
#include <SPI.h>
#include <avr/dtostrf.h>
#include <utility/wifi_drv.h>
#include <WiFiNINA.h>
#include <WiFiUdp.h>
#include <RTCZero.h>
#include <SimpleDHT.h>
#include <NTP.h>
#include <Base64.h>
#include <ArduinoJson.h>

/*  You need to go into .pio/mkrwifi1010/PubSubClient/src/PubSubClient.h and change this line from:
      #define MQTT_MAX_PACKET_SIZE 128
    to:
      #define MQTT_MAX_PACKET_SIZE 2048
*/
#include <PubSubClient.h>

// change the values for Wi-Fi, Azure IoT Central device, and DHT sensor in this file
#include "./configure.h"

#include "./sha256.h"
#include "./morse_code.h"
#include "./utils.h"

int getHubHostName(char *scopeId, char *deviceId, char *key, char *hostName);

enum dht_type
{
    simulated,
    dht22,
    dht11
};

#if defined DHT22_TYPE
SimpleDHT22 dhtSensor(pinDHT);
dht_type dhtType = dht22;
#elif defined DHT11_TYPE
SimpleDHT11 dhtSensor(pinDHT);
dht_type dhtType = dht11;
#else
dht_type dhtType = simulated;
#endif

String iothubHost;
String deviceId;
String sharedAccessKey;

WiFiSSLClient wifiClient;
PubSubClient *mqtt_client = NULL;

bool timeSet = false;
bool wifiConnected = false;
bool mqttConnected = false;

time_t this_second = 0;
time_t checkTime = 1300000000;

#define TELEMETRY_SEND_INTERVAL 60000 // telemetry data sent every 60 seconds
#define PROPERTY_SEND_INTERVAL 60000  // property data sent every 60 seconds
#define SENSOR_READ_INTERVAL 2500     // read sensors every 2.5 seconds

long lastTelemetryMillis = 0;
long lastPropertyMillis = 0;
long lastSensorReadMillis = 0;

float tempValue = 0.0;
float humidityValue = 0.0;
int dieNumberValue = 1;

// MQTT publish topics
static const char PROGMEM IOT_EVENT_TOPIC[] = "devices/{device_id}/messages/events/";
static const char PROGMEM IOT_TWIN_REPORTED_PROPERTY[] = "$iothub/twin/PATCH/properties/reported/?$rid={request_id}";
static const char PROGMEM IOT_TWIN_REQUEST_TWIN_TOPIC[] = "$iothub/twin/GET/?$rid={request_id}";
static const char PROGMEM IOT_DIRECT_METHOD_RESPONSE_TOPIC[] = "$iothub/methods/res/{status}/?$rid={request_id}";

// MQTT subscribe topics
static const char PROGMEM IOT_TWIN_RESULT_TOPIC[] = "$iothub/twin/res/#";
static const char PROGMEM IOT_TWIN_DESIRED_PATCH_TOPIC[] = "$iothub/twin/PATCH/properties/desired/#";
static const char PROGMEM IOT_C2D_TOPIC[] = "devices/{device_id}/messages/devicebound/#";
static const char PROGMEM IOT_DIRECT_MESSAGE_TOPIC[] = "$iothub/methods/POST/#";

int requestId = 0;
int twinRequestId = -1;

// create a WiFi UDP object for NTP to use
WiFiUDP wifiUdp;
// create an NTP object
NTP ntp(wifiUdp);
// Create an rtc object
RTCZero rtc;

#include "./iotc_dps.h"

// get the time from NTP and set the real-time clock on the MKR10x0
void getTime()
{
    Serial.println(F("Getting the time from time service: "));

    ntp.begin();
    ntp.update();
    Serial.print(F("Current UTC time: "));
    Serial.print(ntp.formattedTime("%d. %B %Y - "));
    Serial.println(ntp.formattedTime("%A %T"));

    rtc.begin();
    rtc.setEpoch(ntp.epoch());
    timeSet = true;
}

void acknowledgeSetting(const char* propertyKey, const char* propertyValue, int version)
{
    // for IoT Central need to return acknowledgement
    const static char *PROGMEM responseTemplate = "{\"%s\":{\"value\":%s,\"statusCode\":%d,\"status\":\"%s\",\"desiredVersion\":%d}}";
    char payload[1024];
    sprintf(payload, responseTemplate, propertyKey, propertyValue, 200, F("completed"), version);
    Serial_printf("Sending acknowledgement: %s\n\n", payload);
    String topic = (String)IOT_TWIN_REPORTED_PROPERTY;
    char buff[20];
    topic.replace(F("{request_id}"), itoa(requestId, buff, 10));
    mqtt_client->publish(topic.c_str(), payload);
    requestId++;
}

// In Device template, create a Command called ECHO with a String parameter called displayedValue.
void handleDirectMethod(String topicStr, String payloadStr)
{
    String msgId = topicStr.substring(topicStr.indexOf("$RID=") + 5);
    String methodName = topicStr.substring(topicStr.indexOf(F("$IOTHUB/METHODS/POST/")) + 21, topicStr.indexOf("/?$"));
    Serial_printf((char *)F("Direct method call:\n\tMethod Name: %s\n\tParameters: %s\n"), methodName.c_str(), payloadStr.c_str());
    if (strcmp(methodName.c_str(), "ECHO") == 0)
    {
        // acknowledge receipt of the command
        String response_topic = (String)IOT_DIRECT_METHOD_RESPONSE_TOPIC;
        char buff[20];
        response_topic.replace(F("{request_id}"), msgId);
        response_topic.replace(F("{status}"), F("200")); // OK
        mqtt_client->publish(response_topic.c_str(), "");

        digitalWrite(LED_BUILTIN, HIGH);
        delay(1000);
        digitalWrite(LED_BUILTIN, LOW);

        // output the message as morse code
        const char *msg = payloadStr.c_str();
        morse_encodeAndFlash(msg);
    }
}

void handleCloud2DeviceMessage(String topicStr, String payloadStr)
{
    Serial_printf((char *)F("Cloud to device call:\n\tPayload: %s\n"), payloadStr.c_str());
}

void handleTwinPropertyChange(String topicStr, String payloadStr)
{
    Serial.println(payloadStr);
    // const size_t capacity = JSON_OBJECT_SIZE(5) + JSON_ARRAY_SIZE(2) + 60;
    StaticJsonDocument<200> doc;

    String propertyKey;
    String propertyValueStr;
    int propertyValueNum;
    bool propertyValueBool;
    int version;

    DeserializationError error = deserializeJson(doc, payloadStr);

    if (error)
    {
        Serial.print("deserializeJson() failed: ");
        Serial.println(error.c_str());
        return;
    }

    JsonObject obj = doc.as<JsonObject>();

    if (obj.containsKey("fanSpeed"))
    {
        propertyKey = "fanSpeed";
        propertyValueNum = doc["fanSpeed"];
        propertyValueStr = String(propertyValueNum);
    }
    else if (obj.containsKey("setVoltage"))
    {
        propertyKey = "setVoltage";
        propertyValueNum = doc["setVoltage"];
        propertyValueStr = String(propertyValueNum);
    }
    else if (obj.containsKey("setCurrent"))
    {
        propertyKey = "setCurrent";
        propertyValueNum = doc["setCurrent"];
        propertyValueStr = String(propertyValueNum);
    }
    else if (obj.containsKey("activateIR"))
    {
     propertyKey = "activateIR";
     propertyValueBool = doc["activateIR"];
     propertyValueStr = doc["activateIR"] ? "true" : "false";
    }
    else
    {
        Serial.println("UnKnown!");
    }

    version = doc["$version"];
    acknowledgeSetting(propertyKey.c_str(), propertyValueStr.c_str(), version);
}

// callback for MQTT subscriptions
void callback(char *topic, byte *payload, unsigned int length)
{
    String topicStr = (String)topic;
    topicStr.toUpperCase();
    String payloadStr = (String)((char *)payload);
    payloadStr.remove(length);

    if (topicStr.startsWith(F("$IOTHUB/METHODS/POST/")))
    { // direct method callback
        handleDirectMethod(topicStr, payloadStr);
    }
    else if (topicStr.indexOf(F("/MESSAGES/DEVICEBOUND/")) > -1)
    { // cloud to device message
        handleCloud2DeviceMessage(topicStr, payloadStr);
    }
    else if (topicStr.startsWith(F("$IOTHUB/TWIN/PATCH/PROPERTIES/DESIRED")))
    { // digital twin desired property change
        handleTwinPropertyChange(topicStr, payloadStr);
    }
    else if (topicStr.startsWith(F("$IOTHUB/TWIN/RES")))
    { // digital twin response
        int result = atoi(topicStr.substring(topicStr.indexOf(F("/RES/")) + 5, topicStr.indexOf(F("/?$"))).c_str());
        int msgId = atoi(topicStr.substring(topicStr.indexOf(F("$RID=")) + 5, topicStr.indexOf(F("$VERSION=")) - 1).c_str());
        if (msgId == twinRequestId)
        {
            // twin request processing
            twinRequestId = -1;
            // output limited to 128 bytes so this output may be truncated
            Serial_printf((char *)F("Current state of device twin:\n\t%s"), payloadStr.c_str());
            Serial.println();
        }
        else
        {
            if (result >= 200 && result < 300)
            {
                Serial_printf((char *)F("--> IoT Hub acknowledges successful receipt of twin property: %d\n"), msgId);
            }
            else
            {
                Serial_printf((char *)F("--> IoT Hub could not process twin property: %d, error: %d\n"), msgId, result);
            }
        }
    }
    else
    { // unknown message
        Serial_printf((char *)F("Unknown message arrived [%s]\nPayload contains: %s"), topic, payloadStr.c_str());
    }
}

// connect to Azure IoT Hub via MQTT
void connectMQTT(String deviceId, String username, String password)
{
    WiFiDrv::digitalWrite(26, LOW);  // RED LED
    WiFiDrv::digitalWrite(27, HIGH); // BLUE LED
    mqtt_client->disconnect();

    Serial.println(F("Starting IoT Hub connection"));
    int retry = 0;
    while (retry < 10 && !mqtt_client->connected())
    {
        if (mqtt_client->connect(deviceId.c_str(), username.c_str(), password.c_str()))
        {
            Serial.println(F("===> mqtt connected"));
            mqttConnected = true;
        }
        else
        {
            Serial.print(F("---> mqtt failed, rc="));
            Serial.println(mqtt_client->state());
            delay(2000);
            retry++;
        }
    }
    WiFiDrv::digitalWrite(27, LOW);  // BLUE LED
    WiFiDrv::digitalWrite(25, HIGH); // GREEN LED
}

// create an IoT Hub SAS token for authentication
String createIotHubSASToken(char *key, String url, long expire)
{
    url.toLowerCase();
    String stringToSign = url + "\n" + String(expire);
    int keyLength = strlen(key);

    int decodedKeyLength = base64_dec_len(key, keyLength);
    char decodedKey[decodedKeyLength];

    base64_decode(decodedKey, key, keyLength);

    Sha256 *sha256 = new Sha256();
    sha256->initHmac((const uint8_t *)decodedKey, (size_t)decodedKeyLength);
    sha256->print(stringToSign);
    char *sign = (char *)sha256->resultHmac();
    int encodedSignLen = base64_enc_len(HASH_LENGTH);
    char encodedSign[encodedSignLen];
    base64_encode(encodedSign, sign, HASH_LENGTH);
    delete (sha256);

    return (char *)F("SharedAccessSignature sr=") + url + (char *)F("&sig=") + urlEncode((const char *)encodedSign) + (char *)F("&se=") + String(expire);
}

// reads the value from the DHT sensor if present else generates a random value
void readSensors()
{
    dieNumberValue = random(1, 7);

#if defined DHT11_TYPE || defined DHT22_TYPE
    int err = SimpleDHTErrSuccess;
    if ((err = dhtSensor.read2(&tempValue, &humidityValue, NULL)) != SimpleDHTErrSuccess)
    {
        Serial_printf("Read DHT sensor failed (Error:%d)", err);
        tempValue = -999.99;
        humidityValue = -999.99;
    }
#else
    tempValue = random(0, 7500) / 100.0;
    humidityValue = random(0, 9999) / 100.0;
#endif
}

//////////////////////////////////////////////// SETUP ////////////////////////////////////////////////////////////
void setup()
{
    Serial.begin(115200);

    // uncomment this line to add a small delay to allow time for connecting serial moitor to get full debug output
    delay(5000);

    Serial_printf((char *)F("Hello, starting up the %s device\n"), DEVICE_NAME);

    // RGB LEDS on board
    WiFiDrv::pinMode(25, OUTPUT);
    WiFiDrv::pinMode(26, OUTPUT);
    WiFiDrv::pinMode(27, OUTPUT);
    pinMode(LED_BUILTIN, OUTPUT);

    // seed pseudo-random number generator for die roll and simulated sensor values
    randomSeed(millis());

    // attempt to connect to Wifi network:
    Serial.print((char *)F("WiFi Firmware version is "));
    Serial.println(WiFi.firmwareVersion());
    int status = WL_IDLE_STATUS;
    while (status != WL_CONNECTED)
    {
        Serial_printf((char *)F("Attempting to connect to Wi-Fi SSID: %s \n"), wifi_ssid);
        WiFiDrv::digitalWrite(26, HIGH); // RED LED
        status = WiFi.begin(wifi_ssid, wifi_password);
        delay(5000);
    }

    // get current UTC time
    getTime();

    Serial.println("Getting IoT Hub host from Azure IoT DPS");
    deviceId = iotc_deviceId;
    sharedAccessKey = iotc_deviceKey;
    char hostName[64] = {0};
    getHubHostName((char *)iotc_scopeId, (char *)iotc_deviceId, (char *)iotc_deviceKey, hostName);
    iothubHost = hostName;
    Serial.print("IoT HostName: ");
    Serial.println(hostName);

    // create SAS token and user name for connecting to MQTT broker
    String url = iothubHost + urlEncode(String((char *)F("/devices/") + deviceId).c_str());
    char *devKey = (char *)sharedAccessKey.c_str();
    long expire = rtc.getEpoch() + 864000;
    String sasToken = createIotHubSASToken(devKey, url, expire);
    String username = iothubHost + "/" + deviceId + (char *)F("/api-version=2016-11-14");

    // connect to the IoT Hub MQTT broker
    wifiClient.connect(iothubHost.c_str(), 8883);
    mqtt_client = new PubSubClient(iothubHost.c_str(), 8883, wifiClient);
    connectMQTT(deviceId, username, sasToken);
    mqtt_client->setCallback(callback);

    // add subscriptions
    mqtt_client->subscribe(IOT_TWIN_RESULT_TOPIC);        // twin results
    mqtt_client->subscribe(IOT_TWIN_DESIRED_PATCH_TOPIC); // twin desired properties
    String c2dMessageTopic = IOT_C2D_TOPIC;
    c2dMessageTopic.replace(F("{device_id}"), deviceId);
    mqtt_client->subscribe(c2dMessageTopic.c_str());  // cloud to device messages
    mqtt_client->subscribe(IOT_DIRECT_MESSAGE_TOPIC); // direct messages

    // request full digital twin update
    String topic = (String)IOT_TWIN_REQUEST_TWIN_TOPIC;
    char buff[20];
    topic.replace(F("{request_id}"), itoa(requestId, buff, 10));
    twinRequestId = requestId;
    requestId++;
    mqtt_client->publish(topic.c_str(), "");

    // initialize timers
    lastTelemetryMillis = millis();
    lastPropertyMillis = millis();
}

///////////////////////////////////////////  LOOP ////////////////////////////////////////////////////////
void loop()
{
    mqtt_client->loop();

    // read the sensor values and green LED blink every 2.5 seconds
    if (mqtt_client->connected() && millis() - lastSensorReadMillis > SENSOR_READ_INTERVAL)
    {
        readSensors();
        WiFiDrv::digitalWrite(25, HIGH);
        delay(50);
        WiFiDrv::digitalWrite(25, LOW);

        lastSensorReadMillis = millis();
    }

    // send telemetry values every 5 seconds
    if (mqtt_client->connected() && millis() - lastTelemetryMillis > TELEMETRY_SEND_INTERVAL)
    {
        Serial.println(F("Sending telemetry ..."));
        String topic = (String)IOT_EVENT_TOPIC;
        topic.replace(F("{device_id}"), deviceId);
        char buff[10];
        String payload = F("{\"temp\": {temp}, \"humidity\": {humidity}}");
        payload.replace(F("{temp}"), dtostrf(tempValue, 7, 2, buff));
        payload.replace(F("{humidity}"), dtostrf(humidityValue, 7, 2, buff));
        Serial_printf("\t%s\n", payload.c_str());
        mqtt_client->publish(topic.c_str(), payload.c_str());

        lastTelemetryMillis = millis();
    }

    // send a property update every 15 seconds
    if (mqtt_client->connected() && millis() - lastPropertyMillis > PROPERTY_SEND_INTERVAL)
    {
        Serial.println(F("Sending digital twin property ..."));

        String topic = (String)IOT_TWIN_REPORTED_PROPERTY;
        char buff[20];
        topic.replace(F("{request_id}"), itoa(requestId, buff, 10));
        String payload = F("{\"dieNumber\": {dieNumberValue}}");
        payload.replace(F("{dieNumberValue}"), itoa(dieNumberValue, buff, 10));

        mqtt_client->publish(topic.c_str(), payload.c_str());
        requestId++;

        lastPropertyMillis = millis();
    }
}
