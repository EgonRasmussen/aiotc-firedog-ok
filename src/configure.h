/*
    Virker med WiFiNINA firmware v. 1.5.0
    Tilsyneladende behøver man ikke installere ekstra rodcertifikater
*/ 

// Azure IoT Central device information
static const char PROGMEM iotc_scopeId[] = "0ne0092420A";
static const char PROGMEM iotc_deviceId[] = "MKRWiFi1010";
static const char PROGMEM iotc_deviceKey[] = "UqbS6wPmb5k8QOkz1vn6qHUylwQf7aov+dM4xGJd+Dg=";
//static const char PROGMEM iotc_hubHost[] = "iotc-4f14026e-7a3c-4640-8918-7eb166d81b10.azure-devices.net";

// Wi-Fi information
static char PROGMEM wifi_ssid[] = "SibirienAP";
static char PROGMEM wifi_password[] = "Siberia51244";

#define DEVICE_NAME "Arduino MKR1010"

// comment / un-comment the correct sensor type being used
//#define SIMULATE_DHT_TYPE
//#define DHT11_TYPE
#define DHT22_TYPE

// pin for DHT11/22, 
int pinDHT = 7;
