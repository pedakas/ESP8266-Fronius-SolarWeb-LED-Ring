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
 *   - Blau/Gelb/Orange/Rot: Batterieladestand in Vierteln (je 3 LEDs)
 *   - Eine rote LED: Fehler (WLAN oder API)
 *   - Nachts automatisch aus (Offsets einstellbar; IMMER_AN=1 deaktiviert die Automatik)
 *   - Updates kabellos per OTA (Hostname: siehe HOSTNAME im Sketch)
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

// Zugangsdaten fuer die Web-OTA-Seite (http://<HOSTNAME>.local/update)
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
#define LED_OFFSET 0     // Startpunkt der Anzeige drehen (0-11): 0 = erste LED am Daten-Eingang
#define LED_COLOR_ORDER NEO_GRB  // NEO_GRB = Standard (WS2812B); auf NEO_RGB aendern, falls Rot/Gruen vertauscht erscheinen

// Anzeige-Zeiten: Minuten relativ zum Sonnenauf-/-untergang
#define MORGEN_OFFSET_MIN 30   // Einschalten: so viele Minuten VOR Sonnenaufgang
#define ABEND_OFFSET_MIN 0     // Ausschalten: so viele Minuten NACH Sonnenuntergang (0 = mit Sonnenuntergang)
#define IMMER_AN 0             // 1 = Tag/Nacht-Automatik deaktivieren: Anzeige rund um die Uhr an

// Geraetename im Netzwerk (Router-Anzeige, OTA in der IDE, Web-OTA-URL)
// Bei MEHREREN SolarPuls-Geraeten im selben WLAN muss jedes einen
// EINDEUTIGEN Namen haben: pro Geraet anpassen (z. B. "solarpuls-2")
// oder HOSTNAME_MAC_SUFFIX auf 1 stellen.
#define HOSTNAME "solarpuls"
#define HOSTNAME_MAC_SUFFIX 0  // 1 = automatisch Chip-ID/MAC-Suffix anhaengen (z. B. solarpuls-24f2ab)

Dusk2Dawn standort(latitude, longitude, timeZone);
Adafruit_NeoPixel ring = Adafruit_NeoPixel(NUM_LEDS, LED_PIN, LED_COLOR_ORDER + NEO_KHZ800);

const char* ntpServer = "pool.ntp.org";
// Zeitzonenregel Mitteleuropa inkl. AUTOMATISCHER Sommerzeitumstellung
// (siehe https://github.com/nayarsystems/posix_tz_db)
const char* TZ_RULE = "CET-1CEST,M3.5.0/02,M10.5.0/03";

unsigned long lastTime = 0;
const unsigned long timerDelay = 240000;       // 4 Minuten: Normalfall
const unsigned long timerDelayRetry = 60000;  // 1 Minute: schneller Retry nach Fehler
bool letzterAbrufFehlerhaft = false;          // steuert das Retry-Intervall

// Web-OTA (lauft parallel zum klassischen ArduinoOTA)
ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;

// Pixel mit Drehung setzen: LED_OFFSET verschiebt den Startpunkt der Anzeige,
// falls die erste LED physisch an einer unguenstigen Stelle sitzt.
void setRingPixel(int i, uint32_t color) {
  ring.setPixelColor((i + LED_OFFSET) % NUM_LEDS, color);
}

void setup() {
  Serial.begin(115200);
  
  ring.begin();
  ring.setBrightness(BRIGHTNESS);
  ring.show(); 
  
  // Eindeutigen Geraetenamen setzen (Router-Anzeige, mDNS, OTA)
  char hostname[32];
#if HOSTNAME_MAC_SUFFIX
  snprintf(hostname, sizeof(hostname), HOSTNAME "-%06X", ESP.getChipId());
#else
  snprintf(hostname, sizeof(hostname), "%s", HOSTNAME);
#endif
  WiFi.hostname(hostname);   // Name, den der Router anzeigt
  
  WiFi.begin(ssid, password);
  
  int loadingPos = 0;
  while (WiFi.status() != WL_CONNECTED) {
    ring.clear();
    setRingPixel(loadingPos, ring.Color(0, 0, 255)); 
    ring.show();
    loadingPos = (loadingPos + 1) % NUM_LEDS;
    delay(200);
  }
  
  // Zeit per NTP holen. WICHTIG: Sommerzeit automatisch ueber TZ_RULE
  // statt zwei fester Offsets (die alte Variante lief im Winter genau
  // eine Stunde vor, dadurch schaltete die Ampel zu frueh auf Nacht).
  // Signatur ab ESP8266-Core 3.x: configTime(tzRegel, ntpServer, ...)
  configTime(TZ_RULE, ntpServer);
  
  ArduinoOTA.setHostname(hostname);   // OTA-/mDNS-Name -> http://<HOSTNAME>.local/update 
  ArduinoOTA.begin();
  
  // Web-OTA: Update-Seite unter http://<HOSTNAME>.local/update
  // (Basic-Auth mit den Zugangsdaten oben; MDNS laeuft ueber ArduinoOTA)
  httpUpdater.setup(&httpServer, "/update", otaWebUser, otaWebPassword);
  httpServer.begin();
  
  ring.clear();
  ring.show();
  
  if (isDaytime()) {
    letzterAbrufFehlerhaft = !fetchAndDisplayData();
  }
}

void loop() {
  ArduinoOTA.handle();
  httpServer.handleClient();   // Web-OTA: Browser-Anfragen bedienen
  
  // Nach einem Fehlerhaften Abruf oefter versuchen (1 Minute),
  // sonst im normalen Intervall (4 Minuten).
  if ((millis() - lastTime) > (letzterAbrufFehlerhaft ? timerDelayRetry : timerDelay)) {
    if (isDaytime()) {
      if (WiFi.status() == WL_CONNECTED) {
        letzterAbrufFehlerhaft = !fetchAndDisplayData();
      } else {
        showOfflineError();
        letzterAbrufFehlerhaft = true;
      }
    } else {
      ring.clear();
      ring.show();
      letzterAbrufFehlerhaft = false; // nachts kein Retry-Stau
    }
    lastTime = millis();
  }
}

// Tag/Nacht-Erkennung per NTP-Zeit und Sonnenauf-/untergang
bool isDaytime() {
#if IMMER_AN
  // Testmodus (IMMER_AN=1): immer "Tag" melden, die Automatik ist
  // deaktiviert und die Anzeige laeuft rund um die Uhr.
  // Bei IMMER_AN=0 wird dieser Zweig gar nicht erst kompiliert.
  return true;
#endif
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  // NTP noch nicht synchronisiert -> vorsichtshalber "Tag" annehmen
  if (timeinfo.tm_year < (2026 - 1900)) return true;

  bool isDST = (timeinfo.tm_isdst > 0);
  int sunriseMinutes = standort.sunrise(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, isDST);
  int sunsetMinutes  = standort.sunset(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, isDST);
  int currentMinutes = (timeinfo.tm_hour * 60) + timeinfo.tm_min;

  int einschaltZeit  = sunriseMinutes - MORGEN_OFFSET_MIN;
  int ausschaltZeit  = sunsetMinutes  + ABEND_OFFSET_MIN;

  return (currentMinutes >= einschaltZeit && currentMinutes <= ausschaltZeit);
}

// Eine rote LED = Fehler (kein WLAN oder API-/JSON-Fehler)
void showOfflineError() {
  ring.clear();
  setRingPixel(0, ring.Color(255, 0, 0)); 
  ring.show();
}

bool fetchAndDisplayData() {
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
        // SOC oder P_Grid fehlen bzw. sind null (z. B. kurz vor 100 %
        // Batterieladung beim Umschalten des Wechselrichters) -> als
        // Fehler behandeln, statt stiller Dunkelheit anzuzeigen.
        if (doc["SOC"].is<float>() && doc["P_Grid"].is<float>()) {
          float p_grid = doc["P_Grid"];
          float soc = doc["SOC"];
          updateLeds(p_grid, soc);
          success = true;
        }
      }
    }
    https.end();
  }

  if (!success) {
    showOfflineError();
  }
  return success;
}

void updateLeds(float grid, float soc) {
  bool losGehts = (grid <= -1000.0) && (soc >= 98.0);
  ring.clear(); 

  if (losGehts) {
    for(int i = 0; i < NUM_LEDS; i++) {
      setRingPixel(i, ring.Color(0, 255, 0)); 
    }
  } else {
    int activeLeds = round((soc / 100.0) * NUM_LEDS);
    if (activeLeds > NUM_LEDS) activeLeds = NUM_LEDS;
    
    // 4 Zonen à 3 LEDs (= Viertel des Ladestands)
    uint32_t batColor;
    if (soc <= 25.0) {
      batColor = ring.Color(255, 0, 0);      // Rot:    0-25 %
    } else if (soc <= 50.0) {
      batColor = ring.Color(255, 120, 0);   // Orange: 25-50 %
    } else if (soc <= 75.0) {
      batColor = ring.Color(255, 255, 0);   // Gelb:   50-75 %
    } else {
      batColor = ring.Color(0, 0, 255);     // Blau:   75-100 %
    }
    
    for(int i = 0; i < activeLeds; i++) {
      setRingPixel(i, batColor); 
    }
  }
  ring.show(); 
}
