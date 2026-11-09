/*
 * SolarPuls: Fronius SolarWeb PV-Anzeige auf einem 12er WS2812B LED-Ring
 * Hardware: Wemos D1 Mini (ESP8266) + X-Ring (12x WS2812B)
 *
 * Verkabelung:
 *   D1 Mini 5V  -> X-Ring 5V
 *   D1 Mini GND -> X-Ring GND / 5VGND
 *   D1 Mini D4  -> X-Ring RXD-Pad (optional 330-470 Ohm)
 *   WICHTIG: DIP-Schalter 7 (RXD) auf ON stellen - nur EIN Schalter!
 *   (Der DIP-Schalter waehlt, welchen Pad die LED-Datenleitung bekommt.
 *    Siehe https://no3x.de/projects/x-ring/x-ring-missing-documentation)
 *
 * Funktion:
 *   - Ruft alle 4 Minuten die Solar.web Public-Display-API ab
 *   - Gruen: >1 kW Einspeisung UND Batterie >= 98 %
 *   - Blau/Orange/Rot: Batterieladestand (jede LED ~8,3 %)
 *   - Eine rote LED: Fehler (WLAN oder API)
 *   - Nachts automatisch aus (Sonnenauf-/untergang +/- 30 min)
 *   - Updates kabellos per OTA (Hostname: "solarpuls")
 *
 * Benoetigt: Adafruit NeoPixel, ArduinoJson 7.x, Dusk2Dawn
 * Board: LOLIN(WEMOS) D1 R1 & mini
 */

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Dusk2Dawn.h> 
#include <Adafruit_NeoPixel.h> 
#include <ArduinoOTA.h> 
#include <ESP8266WebServer.h>        // Web-OTA: Update-Seite im Browser
#include <ESP8266HTTPUpdateServer.h>  // Web-OTA: Firmware-Upload per HTTP

// ==========================================
// BITTE HIER DEINE DATEN EINTRAGEN
// ==========================================
const char* ssid = "DEIN_WLAN_NAME";
const char* password = "DEIN_WLAN_PASSWORT";

// Zugangsdaten fuer die Web-OTA-Seite (http://solarpuls.local/update)
// BITTE UNBEDINGT AENDERN, sonst kann jeder im Netzwerk flashen!
const char* otaWebUser = "admin";
const char* otaWebPassword = "BITTE_AENDERN";

const char* publicDisplayUrl = "https://www.solarweb.com/ActualData/GetCompareDataForPublicDisplay?PublicDisplayToken=DEIN_TOKEN";

// Standort fuer automatische Tag/Nacht-Erkennung
// BITTE HIER DEINE KOORDINATEN EINTRAGEN (Dezimalgrad, z. B. aus Google Maps).
// Hinweis: Ohne Eintrag funktioniert die Tag/Nacht-Automatik nicht korrekt.
float latitude = 0.000000;   // <- Breitengrad HIER EINTRAGEN
float longitude = 0.000000;  // <- Laengengrad HIER EINTRAGEN
// timeZone = Standardzeit OHNE Sommerzeit (Oesterreich/Deutschland: 1)
int timeZone = 1; 

// ==========================================
// LED-EINSTELLUNGEN FUER DIE VERKABELUNG
// ==========================================
#define LED_PIN D4       // D4 = GPIO2; alternativ D2, D1, D5 oder D7
#define NUM_LEDS 12      // Der X-Ring hat 12 LEDs
#define BRIGHTNESS 50    

Dusk2Dawn standort(latitude, longitude, timeZone);
Adafruit_NeoPixel ring = Adafruit_NeoPixel(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600;
const int   daylightOffset_sec = 3600;

unsigned long lastTime = 0;
const unsigned long timerDelay = 240000; // 4 Minuten

// Web-OTA (lauft parallel zum klassischen ArduinoOTA)
ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;

void setup() {
  Serial.begin(115200);
  
  ring.begin();
  ring.setBrightness(BRIGHTNESS);
  ring.show(); 
  
  WiFi.begin(ssid, password);
  
  int loadingPos = 0;
  while (WiFi.status() != WL_CONNECTED) {
    ring.clear();
    ring.setPixelColor(loadingPos, ring.Color(0, 0, 255)); 
    ring.show();
    loadingPos = (loadingPos + 1) % NUM_LEDS;
    delay(200);
  }
  
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  ArduinoOTA.setHostname("solarpuls"); 
  ArduinoOTA.begin();
  
  // Web-OTA: Update-Seite unter http://solarpuls.local/update
  // (Basic-Auth mit den Zugangsdaten oben; MDNS laeuft ueber ArduinoOTA)
  httpUpdater.setup(&httpServer, "/update", otaWebUser, otaWebPassword);
  httpServer.begin();
  
  ring.clear();
  ring.show();
  
  if (isDaytime()) {
    fetchAndDisplayData();
  }
}

void loop() {
  ArduinoOTA.handle();
  httpServer.handleClient();   // Web-OTA: Browser-Anfragen bedienen
  
  if ((millis() - lastTime) > timerDelay) {
    if (isDaytime()) {
      if (WiFi.status() == WL_CONNECTED) {
        fetchAndDisplayData();
      } else {
        showOfflineError();
      }
    } else {
      ring.clear();
      ring.show();
    }
    lastTime = millis();
  }
}

// Tag/Nacht-Erkennung per NTP-Zeit und Sonnenauf-/untergang
bool isDaytime() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  // NTP noch nicht synchronisiert -> vorsichtshalber "Tag" annehmen
  if (timeinfo.tm_year < (2026 - 1900)) return true;

  bool isDST = (timeinfo.tm_isdst > 0);
  int sunriseMinutes = standort.sunrise(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, isDST);
  int sunsetMinutes  = standort.sunset(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, isDST);
  int currentMinutes = (timeinfo.tm_hour * 60) + timeinfo.tm_min;

  int einschaltZeit  = sunriseMinutes - 30;
  int ausschaltZeit  = sunsetMinutes  + 30;

  return (currentMinutes >= einschaltZeit && currentMinutes <= ausschaltZeit);
}

// Eine rote LED = Fehler (kein WLAN oder API-/JSON-Fehler)
void showOfflineError() {
  ring.clear();
  ring.setPixelColor(0, ring.Color(255, 0, 0)); 
  ring.show();
}

void fetchAndDisplayData() {
  WiFiClientSecure client;
  client.setInsecure(); 
  HTTPClient https;
  bool success = false;

  if (https.begin(client, publicDisplayUrl)) {
    int httpCode = https.GET();
    if (httpCode == HTTP_CODE_OK) {
      String payload = https.getString();
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload);
      
      if (!error) {
        float p_grid = doc["P_Grid"];
        float soc = doc["SOC"];
        updateLeds(p_grid, soc);
        success = true; 
      }
    }
    https.end();
  }

  if (!success) {
    showOfflineError();
  }
}

void updateLeds(float grid, float soc) {
  bool losGehts = (grid <= -1000.0) && (soc >= 98.0);
  ring.clear(); 

  if (losGehts) {
    for(int i = 0; i < NUM_LEDS; i++) {
      ring.setPixelColor(i, ring.Color(0, 255, 0)); 
    }
  } else {
    int activeLeds = round((soc / 100.0) * NUM_LEDS);
    if (activeLeds > NUM_LEDS) activeLeds = NUM_LEDS;
    
    uint32_t batColor;
    if (soc <= 30.0) {
      batColor = ring.Color(255, 0, 0);     
    } else if (soc <= 70.0) {
      batColor = ring.Color(255, 120, 0);   
    } else {
      batColor = ring.Color(0, 0, 255);     
    }
    
    for(int i = 0; i < activeLeds; i++) {
      ring.setPixelColor(i, batColor); 
    }
  }
  ring.show(); 
}
