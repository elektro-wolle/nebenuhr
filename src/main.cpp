/**
 * CTW Nebenuhr - A Smart Clock Controller
 *
 * This project controls a slave clock that displays time by advancing minute-by-minute.
 * The ESP8266 receives accurate time via NTP and drives the clock mechanism to keep
 * it synchronized. It provides a web interface for manual time adjustment and timezone
 * configuration.
 *
 * MIT License
 *
 * Copyright (c) 2025 Wolfgang Jung
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include <Arduino.h>

#define ESP_DRD_USE_EEPROM true

// Core ESP8266 and networking libraries
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESP_DoubleResetDetector.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>

// Time zone handling library
#include <AceTime.h>

#include <list>

#define OTA 1

#if !defined(NTP_SERVER)
#define NTP_SERVER "pool.ntp.org"
#endif

#define DEBUG 1
#define STATS_ADDRESS 10
#define DRD_ADDRESS 4
#define EEPROM_MAGIC_NUMBER 0xdeadbeef

// Structure to persist operational statistics across reboots
typedef struct {
    uint32_t magicNumber; // Validation marker for EEPROM data integrity
    uint32_t uptimeSeconds; // Current session uptime
    uint32_t uptimeSecondsTotal; // Cumulative uptime across all sessions
    uint32_t previousSecondsTotal; // Previous total for calculation purposes
    uint16_t reboots; // Number of device restarts
    uint32_t zoneId; // Currently selected timezone identifier
} statistics_t;

statistics_t globalStats;

// Double reset detection - allows WiFi config reset via rapid power cycling
DoubleResetDetector drd(DRD_ADDRESS, 0);

/**
 * Custom logger that captures serial output for web display
 * Maintains a circular buffer of recent log messages for debugging
 */
class Logger : public Print {
public:
    size_t write(uint8_t c)
    {
        if (c == '\n') {
            // Complete line received - add to history and manage buffer size
            lastItems.push_back(currentLine);
            if (lastItems.size() > 100) {
                lastItems.pop_front();
            }
            currentLine = String("");
        } else {
            currentLine += (char)c;
        }
        return 1;
    }
    std::list<String> lastItems; // Recent log messages for web display

private:
    String currentLine = String("");
};

Logger logger;

// Time constants and timezone management
static const time_t EPOCH_2000_01_01 = 946684800;
static const unsigned long REBOOT_TIMEOUT_MILLIS = 5000;

using namespace ace_time;
using namespace ace_time::zonedbx;
static const int CACHE_SIZE = 3;
ExtendedZoneProcessorCache<1> zoneProcessorCache;
static ExtendedZoneProcessor localZoneProcessor;

// Global timezone manager for handling all world timezones
ExtendedZoneManager zoneManager(
    zonedbx::kZoneRegistrySize,
    zonedbx::kZoneRegistry,
    zoneProcessorCache);

// Default to Central European timezone
static TimeZone localZone = zoneManager.createForZoneInfo(&zonedbx::kZoneEurope_Berlin);

// Web server for configuration interface
ESP8266WebServer server(80);

// Hardware pin assignments for clock control signals
// OUT1 -> D3
// OUT2 -> D4
#define OUT1 D3
#define OUT2 D4

// Time tracking variables (in minutes from midnight)
int16_t currentDisplayedTime = 9 * 60 + 44; // What the physical clock shows
int16_t currentTime = 9 * 60 + 44; // Actual current time

void setCurrentTime();

/**
 * Initialize NTP time synchronization
 * Waits for valid time from internet time servers with timeout protection
 */
void setupSntp()
{
    Serial.print(F("Configuring SNTP"));
    configTime(0 /*timezone*/, 0 /*dst_sec*/, NTP_SERVER);

    // Wait for valid time response (post-Y2K timestamps only)
    unsigned long startMillis = millis();
    while (true) {
        Serial.print('.'); // Progress indicator
        time_t now = time(nullptr);
        if (now >= EPOCH_2000_01_01) {
            Serial.println(F(" Done."));
            break;
        }

        // Prevent infinite hang - reboot if NTP fails
        unsigned long nowMillis = millis();
        if ((unsigned long)(nowMillis - startMillis) >= REBOOT_TIMEOUT_MILLIS) {
            Serial.println(F(" FAILED! Rebooting..."));
            drd.loop();
            delay(1000);
            ESP.reset();
        }

        delay(500);
    }
}

/**
 * Convert seconds to human-readable duration string
 * Formats as "Xd Yh Zm Ws" for display purposes
 */
String secondsToString(uint32_t seconds)
{
    String result = "";
    if (seconds > 86400) {
        result = String(seconds / 86400) + "d ";
    }
    result = result + String((seconds / 3600) % 24) + "h ";
    result = result + String((seconds / 60) % 60) + "m ";
    result = result + String(seconds % 60) + "s";
    return result;
}

/**
 * Generate the main web interface
 * Provides time setting controls, timezone selection, and system status
 */
void handleRoot()
{
    // Convert displayed time from minutes to hours:minutes format
    int hour = (((1440 + currentDisplayedTime) / 60) % 24);
    int minute = (1440 + currentDisplayedTime) % 60;

    server.chunkedResponseModeStart(200, "text/html; charset=utf-8");

    // HTML header and CSS styling for clean interface
    String webpage = F("<!DOCTYPE html><html><head>\n");
    webpage += F("<title>CTW Nebenuhr</title><style>\n");
    webpage += F("body{margin-left:5em;margin-right:5em;font-family:sans-serif;font-size:14px;color:darkslategray;background-color:#EEE}h1{text-align:center}.info{width:100%;text-align:left;font-size:18pt}input,main,option,select,th{font-size:24pt;text-align:left}input{width:100%}input[type='submit']{width:min-content;float:right;text-align:right}main{font-size:16pt;vertical-align:middle}.info{line-height:2em}.info br{margin-left:3em}.logs{margin-top:2em;padding-top:2em;overflow-x:auto;border-top:black 2px solid}ul li{text-align:left}\n");
    webpage += F(".graph {background-color: #EEE; font-size:0; overflow-x: auto; padding-bottom: 40px;} .bar { background-color: blueviolet; width: 1px; display: inline-block; } .active { background-color: green; }");
    webpage += F("</style></head><body><h1>CTW Nebenuhr by Wolfgang Jung</h1><div class='main'>\n");

    // Time adjustment form
    webpage += F("<h2>Aktuell angezeigte Zeit:</h2>\n");
    webpage += F("<form action=\"/set\" method=\"POST\"><table>\n");
    webpage += F("<tr><th>Stunde:</th><td><input type=\"number\" name=\"hour\" value=\"") + String(hour) + F("\" min=\"0\" max=\"23\"></td></tr>");
    webpage += F("<tr><th>Minute:</th><td><input type=\"number\" name=\"minute\" value=\"") + String(minute) + F("\" min=\"0\" max=\"59\"></td></tr>");
    webpage += F("<tr><th>Zeitzone:</th><td><select name='zone'>\n");
    server.sendContent(webpage);

    // Generate sorted timezone dropdown list
    uint16_t indexes[zonedbx::kZoneRegistrySize];
    ace_time::ZoneSorterByName<ExtendedZoneManager> zoneSorter(zoneManager);
    zoneSorter.fillIndexes(indexes, zonedbx::kZoneRegistrySize);
    zoneSorter.sortIndexes(indexes, zonedbx::kZoneRegistrySize);
    for (int i = 0; i < zonedbx::kZoneRegistrySize; i++) {
        ace_common::PrintStr<32> printStr;
        ExtendedZone zone = zoneManager.getZoneForIndex(indexes[i]);
        zone.printNameTo(printStr);
        webpage = "<option value='" + String(indexes[i]) + "'";
        if (zone.zoneId() == globalStats.zoneId) {
            webpage += F(" selected='selected'");
        }
        webpage += ">" + String(printStr.getCstr()) + "</option>\n";
        server.sendContent(webpage);
    }

    webpage = F("</select></td></tr>");
    webpage += F("<tr><th></th><td><input id='save' type=\"submit\" value=\"Speichern\"></td></tr></table></form><br/></div>\n");

    // Current time and system information display
    webpage += F("<div class='info'>");
    webpage += F("<div class='time'><h2>Aktuelle Zeit</h2><tt>");

    time_t localTime = time(nullptr);
    ZonedDateTime zonedDateTime = ZonedDateTime::forUnixSeconds64(
        localTime, localZone);
    ace_common::PrintStr<60> currentTimeStr;
    zonedDateTime.printTo(currentTimeStr);

    webpage += String(currentTimeStr.getCstr()) + "</tt></div></br>\n";

    // System statistics section
    webpage += "<div class='stats'><h2>Stats</h2>\n";
    webpage += "Uptime:" + secondsToString(globalStats.uptimeSeconds) + "<br/>\n";
    webpage += "Uptime gesamt:" + secondsToString(globalStats.uptimeSecondsTotal) + "<br/>\n";
    webpage += "Reboots:" + String(globalStats.reboots) + "<br/>\n";
    webpage += "Version: " + String(__TIMESTAMP__) + "<br/></div></div>\n";
    server.sendContent(webpage);

    // Recent log messages for debugging
    if (logger.lastItems.size() > 0) {
        server.sendContent(F("<div class='logs'><h2>Logs</h2><ul>\n"));

        for (std::list<String>::reverse_iterator line = logger.lastItems.rbegin();
            line != logger.lastItems.rend();
            line++) {
            server.sendContent("<li><pre>" + (*line) + "</pre></li>\n");
        }
        server.sendContent(F("</ul></div>"));
    }

    server.sendContent(F("</body></html>\n"));
    server.chunkedResponseFinalize();
}

/**
 * Process time and timezone setting form submission
 * Updates displayed time and saves new timezone preference
 */
void handleSet()
{
    int hour = server.arg("hour").toInt();
    int minute = server.arg("minute").toInt();
    int zoneIdx = server.arg("zone").toInt();
    currentDisplayedTime = (hour * 60 + minute) % 1440;

    // Update timezone if valid selection made
    localZone = zoneManager.createForZoneIndex(zoneIdx);
    if (localZone.isError() == false) {
        globalStats.zoneId = localZone.getZoneId();
        EEPROM.put(STATS_ADDRESS, globalStats);
        EEPROM.commit();
    }

    // Redirect back to main page
    server.sendHeader(F("Location"), "/");
    server.send(302, F("text/plain"), "");
}

/**
 * Load persistent configuration and statistics from EEPROM
 * Initializes default values if no valid data found
 */
void readFromEEProm()
{
    // EEPROM.begin(sizeof(statistics_t));
    // Already got begin() called by DRD constructor
    EEPROM.get(STATS_ADDRESS, globalStats);

    // Check for valid stored data, initialize defaults if corrupted
    if (globalStats.magicNumber != EEPROM_MAGIC_NUMBER) {
        globalStats.magicNumber = EEPROM_MAGIC_NUMBER;
        globalStats.reboots = 0;
        globalStats.uptimeSeconds = 0;
        globalStats.uptimeSecondsTotal = 0;
        globalStats.previousSecondsTotal = 0;
        globalStats.zoneId = zonedbx::kZoneIdEurope_Berlin;
        EEPROM.put(STATS_ADDRESS, globalStats);
    }

    // Prepare for uptime calculation across sessions
    globalStats.previousSecondsTotal = globalStats.uptimeSecondsTotal;

    // Restore timezone from saved preference
    localZone = zoneManager.createForZoneId(globalStats.zoneId);
    if (localZone.isError()) {
        // Fallback to default if saved timezone invalid
        globalStats.zoneId = zonedbx::kZoneIdEurope_Berlin;
        localZone = zoneManager.createForZoneId(globalStats.zoneId);
        EEPROM.put(STATS_ADDRESS, globalStats);
    }
    globalStats.uptimeSeconds = 0;

    Serial.print(F("Using Timezone: "));
    localZone.printTo(Serial);
    Serial.println();
    EEPROM.commit();
}

/**
 * One-time system initialization
 * Sets up WiFi, web server, NTP sync, and hardware pins
 */
void setup()
{
    Serial.begin(115200);
    readFromEEProm();
    globalStats.uptimeSeconds = 0;

    Serial.println(F("\nStarting CTW Nebenuhr 2025 - Wolfgang Jung / Ideas In Logic\n"));

    // Configure hardware control pins for clock mechanism
    pinMode(OUT1, OUTPUT);
    pinMode(OUT2, OUTPUT);

    // Status LED setup
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);
    digitalWrite(OUT1, LOW);
    digitalWrite(OUT2, LOW);

    WiFiManager wiFiManager;

    // Check for double reset to enter WiFi configuration mode
    if (drd.detectDoubleReset()) {
        digitalWrite(LED_BUILTIN, HIGH);
        Serial.println(F("Reset WiFi configuration"));
        wiFiManager.resetSettings();
        wiFiManager.startConfigPortal("nebenuhr", "");
    }

    // Attempt to connect to previously configured WiFi
#ifdef DEBUG
    logger.println(F("Trying to connect to known WiFi"));
#endif
    if (!wiFiManager.autoConnect("nebenuhr")) {
        digitalWrite(LED_BUILTIN, HIGH);
        Serial.println(F("failed to connect and hit timeout"));
        delay(3000);
        digitalWrite(LED_BUILTIN, LOW);
        // Connection failed - restart and try again
        ESP.reset();
    }

    // Enable local network discovery
    if (MDNS.begin("nebenuhr")) { // Start the mDNS responder for esp8266.local
#ifdef DEBUG
        logger.println(F("mDNS responder started"));
#endif
    } else {
        logger.println(F("Error setting up MDNS responder!"));
    }

    // Configure web server endpoints
    digitalWrite(LED_BUILTIN, HIGH);
    server.on("/", HTTP_GET, handleRoot);
    server.on("/set", HTTP_POST, handleSet);
    server.onNotFound([]() {
        server.send(404, F("text/plain"), F("404: Not found"));
    });
    server.begin();

    // Track this boot in statistics
    globalStats.reboots++;
    EEPROM.put(STATS_ADDRESS, globalStats);

    // Get accurate time from internet
    setupSntp();
    setCurrentTime();

    // Assume clock lost minimal time during power outage
    currentDisplayedTime = currentTime;

#ifdef OTA
    // Enable over-the-air firmware updates
    ArduinoOTA.setPort(8266);

    ArduinoOTA.setHostname("nebenuhr");

    // OTA event handlers for status reporting
    ArduinoOTA.onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH) {
            type = "sketch";
        } else { // U_FS
            type = "filesystem";
        }
        Serial.println("Start updating " + type);
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("\nEnd");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
#ifdef DEBUG
        logger.printf("Progress: %u%%\r", (progress / (total / 100)));
#endif
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) {
            Serial.println("Auth Failed");
        } else if (error == OTA_BEGIN_ERROR) {
            Serial.println("Begin Failed");
        } else if (error == OTA_CONNECT_ERROR) {
            Serial.println("Connect Failed");
        } else if (error == OTA_RECEIVE_ERROR) {
            Serial.println("Receive Failed");
        } else if (error == OTA_END_ERROR) {
            Serial.println("End Failed");
        }
    });
    ArduinoOTA.begin();
#endif
    digitalWrite(LED_BUILTIN, LOW);
}

/**
 * Update current time from NTP source with timezone conversion
 * Accounts for seconds to prevent minute boundary issues
 */
void setCurrentTime()
{
    time_t localTime = time(nullptr);
    ZonedDateTime zonedDateTime = ZonedDateTime::forUnixSeconds64(
        localTime, localZone);
    currentTime = zonedDateTime.minute() + zonedDateTime.hour() * 60;

    // Pre-advance if close to next minute to prevent timing issues
    if (zonedDateTime.second() == 59) {
        currentTime += 1;
    }
}

/**
 * Advance the physical clock by one minute
 * Uses alternating pulses to drive the clock mechanism forward
 */
void advance()
{
    uint8_t STEPS[]={0, 4, 8, 16, 32, 64, 128, 192, 255};
    for (size_t x = 0; x< sizeof(STEPS)/sizeof(STEPS[0]); x++) {
        // Generate alternating pulse pattern for clock drive mechanism
        if (currentDisplayedTime % 2 == 0) {
            analogWrite(OUT1, 255 - STEPS[x]);
            digitalWrite(OUT2, HIGH);
        } else {
            digitalWrite(OUT1, HIGH);
            analogWrite(OUT2, 255 - STEPS[x]);
        }
        delay(30);
    }
    delay(200); // Pulse duration for reliable clock movement
    digitalWrite(OUT1, LOW);
    digitalWrite(OUT2, LOW);

    // Update our tracking of displayed time
    currentDisplayedTime++;
    if (currentDisplayedTime >= 1440) { // Handle midnight rollover
        currentDisplayedTime -= 1440;
    }
}

/**
 * Template function to execute code at regular intervals
 * Prevents blocking delays while maintaining precise timing
 */
template <int T>
void runEvery(void (*f)())
{
    static unsigned long lastMillis = millis();
    unsigned long now = millis();
    if (now >= lastMillis + T) {
        // Execute function every T milliseconds
        lastMillis = millis();
        f();
    }
}

/**
 * Main program loop - handles all ongoing operations
 * Manages web requests, clock synchronization, and system maintenance
 */
void loop()
{
    // Handle incoming web requests
    server.handleClient();
#ifdef OTA
    // Process any OTA update requests
    ArduinoOTA.handle();
#endif

    // Primary clock synchronization logic - runs every second
    runEvery<1000>([]() {
        if (currentDisplayedTime == currentTime) {
            // Clock is synchronized - no action needed
        } else if (currentDisplayedTime < currentTime) {
            // Clock is behind - advance one minute
            advance();
        } else if (currentDisplayedTime > currentTime + 10) {
            // Clock is significantly ahead - reset to previous day for catch-up
            currentDisplayedTime -= 1440;
        }
    });

    // System maintenance tasks - runs every 500ms
    runEvery<500>([]() {
        // Update runtime statistics
        globalStats.uptimeSeconds = (millis() / 1000);
        globalStats.uptimeSecondsTotal = globalStats.previousSecondsTotal + globalStats.uptimeSeconds;

        // Refresh current time from NTP
        setCurrentTime();

        // Service reset detection and network discovery
        drd.loop();
        MDNS.update();
    });

    // Periodic data persistence - runs every 15 minutes
    runEvery<1000 * 15 * 60>([]() {
        // Save current statistics to survive reboots
        EEPROM.put(STATS_ADDRESS, globalStats);
        EEPROM.commit();
    });
}
