#include "config.h"

// device
String g_hostname = "espgarden";
String g_timezone = "<-03>3";

// wifi
String g_ssid = "undefined";
String g_wifiPassword = "undefined";

// over the air update
String g_otaUser = "admin";
String g_otaPassword = "password";

// thingspeak
String g_thingSpeakAPIKey = "undefined";
long g_thingSpeakChannelNumber = 0;

// talkback
String g_talkBackAPIKey = "undefined";
long g_talkBackID = 0;

// pins
uint8_t g_buttonPin = 0;
uint8_t g_wateringPin = 15;
uint8_t g_wateringPinOn = 0;
uint8_t g_dhtPin = 23;
uint8_t g_soilMoisturePin = A0;
uint8_t g_luminosityPin = A3;
