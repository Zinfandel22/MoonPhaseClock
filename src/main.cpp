/**
 * Moon Phase
 * Seeed Studio XIAO ESP32C3 + GC9A01 240x240 round (SPI, TFT_eSPI) + LVGL 8.3.11
 * Credit to nishad2m8 https://github.com/nishad2m8
 * What this does:
 *  - Connects to WiFi, syncs time via NTP
 *  - Updates the time/date labels every second from the system clock
 *  - Periodically fetches the current moon age from NASA's Dial-a-Moon API
 *    and updates the phase name label + moon image accordingly
 *
 * NOTE: TFT pin mapping lives in platformio.ini build_flags.
 * Configure Wi-Fi, location, and time zone through the setup access point on first boot.
 */

#include <Arduino.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include "ui.h"

TFT_eSPI tft = TFT_eSPI();

const char *NTP_SERVER = "pool.ntp.org";

struct DeviceConfig {
    String ssid;
    String password;
    String timezone;
    double latitude;
    double longitude;
};

static DeviceConfig deviceConfig;
static WebServer configServer(80);
static DNSServer dnsServer;
static const char *CONFIG_NAMESPACE = "moonclock";
static const uint16_t DNS_PORT = 53;

// ---- LVGL display buffer (partial, single buffer, internal RAM only) ----
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[240 * 40];
static lv_disp_drv_t disp_drv;

static void disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)&color_p->full, w * h, true);
    tft.endWrite();

    lv_disp_flush_ready(disp);
}


static const lv_img_dsc_t *moon_frames[30] = {
    &ui_img_moon_moon_1_png,  &ui_img_moon_moon_2_png,  &ui_img_moon_moon_3_png,
    &ui_img_moon_moon_4_png,  &ui_img_moon_moon_5_png,  &ui_img_moon_moon_6_png,
    &ui_img_moon_moon_7_png,  &ui_img_moon_moon_8_png,  &ui_img_moon_moon_9_png,
    &ui_img_moon_moon_10_png, &ui_img_moon_moon_11_png, &ui_img_moon_moon_12_png,
    &ui_img_moon_moon_13_png, &ui_img_moon_moon_14_png, &ui_img_moon_moon_15_png,
    &ui_img_moon_moon_16_png, &ui_img_moon_moon_17_png, &ui_img_moon_moon_18_png,
    &ui_img_moon_moon_19_png, &ui_img_moon_moon_20_png, &ui_img_moon_moon_21_png,
    &ui_img_moon_moon_22_png, &ui_img_moon_moon_23_png, &ui_img_moon_moon_24_png,
    &ui_img_moon_moon_25_png, &ui_img_moon_moon_26_png, &ui_img_moon_moon_27_png,
    &ui_img_moon_moon_28_png, &ui_img_moon_moon_29_png, &ui_img_moon_moon_30_png,
};

// ---- Update timers ----
const uint32_t CLOCK_UPDATE_MS = 1000;                 // refresh time/date label every second
const uint32_t MOON_UPDATE_MS = 60UL * 60UL * 1000UL;  // refetch moon phase hourly once we have a
                                                        // successful reading (moon age barely moves
                                                        // minute to minute)
const uint32_t MOON_RETRY_MS = 15UL * 1000UL;          // retry this often until the FIRST fetch succeeds

static uint32_t lastClockMillis = 0;
static uint32_t lastMoonMillis = 0;
static bool moonDataValid = false;

// Extra label added under the clock face for local moonrise/moonset.
// (Not part of the SquareLine Studio export, so it's safe from re-exports.)
static lv_obj_t *ui_moon_times = NULL;
static lv_obj_t *ui_time_period = NULL;

// ---- Forward declarations ----
bool loadDeviceConfig();
bool saveDeviceConfig(const DeviceConfig &config);
bool isValidConfig(const DeviceConfig &config);
bool connectToWiFi();
void startProvisioningPortal();
void handleConfigurationSave();
String configurationPage(const String &message);
String htmlEscape(const String &value);
bool waitForTimeSync(uint32_t timeoutMs);
void updateClockLabels();
void updateMoonData();
void updateMoonRiseSet();
String getMoonPhase(double age);
int getMoonImageIndex(double age);
void setMoonImage(int index);
void playMoonIntroAnimation();

void setup()
{
    Serial.begin(115200);
    tft.begin();
    tft.setRotation(0); // Try 0 if image is upside down.
    tft.fillScreen(TFT_BLACK);
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, 240 * 40);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = 240;
    disp_drv.ver_res = 240;
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    ui_init();

    ui_time_period = lv_label_create(ui_Screen1);
    lv_obj_set_width(ui_time_period, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_time_period, LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(ui_time_period, lv_color_hex(0xFFFF00), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_time_period, &ui_font_datefont, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(ui_time_period, "am");
    lv_obj_align_to(ui_time_period, ui_time, LV_ALIGN_OUT_RIGHT_BOTTOM, 2, -2);

    ui_moon_times = lv_label_create(ui_Screen1);
    lv_obj_set_width(ui_moon_times, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_moon_times, LV_SIZE_CONTENT);
    // To the right of the time, same vertical neighborhood, moonrise
    // stacked above moonset.
    lv_obj_align(ui_moon_times, LV_ALIGN_CENTER, 68, 38);
    lv_obj_set_style_text_color(ui_moon_times, lv_color_hex(0xDBDBDB), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_moon_times, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_moon_times, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_moon_times, &ui_font_datefont, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(ui_moon_times, "^ --:--\nv --:--");

    bool hasDeviceConfig = loadDeviceConfig();
    if (hasDeviceConfig) {
        lv_img_set_pivot(ui_img_moon, 50, 50);
        lv_img_set_angle(ui_img_moon, deviceConfig.latitude < 0.0 ? 1800 : 0);
    }

    lv_task_handler(); 
    playMoonIntroAnimation();

    if (!hasDeviceConfig) {
        startProvisioningPortal();
    }
    if (!connectToWiFi()) {
        startProvisioningPortal();
    }

    configTzTime(deviceConfig.timezone.c_str(), NTP_SERVER);
    waitForTimeSync(15000); 
    updateMoonData();
    updateMoonRiseSet();
    lastClockMillis = millis();
    lastMoonMillis = millis();
    Serial.println("Setup complete.");
}

void loop()
{
    lv_task_handler();
    uint32_t now = millis();
    if (now - lastClockMillis >= CLOCK_UPDATE_MS) {
        lastClockMillis = now;
        updateClockLabels();
    }

    if (now - lastMoonMillis >= (moonDataValid ? MOON_UPDATE_MS : MOON_RETRY_MS)) {
        lastMoonMillis = now;
        updateMoonData();
        updateMoonRiseSet();
    }

    delay(5);
}

bool loadDeviceConfig()
{
    Preferences preferences;
    if (!preferences.begin(CONFIG_NAMESPACE, true)) return false;

    bool configured = preferences.getBool("configured", false);
    if (configured) {
        deviceConfig.ssid = preferences.getString("ssid", "");
        deviceConfig.password = preferences.getString("password", "");
        deviceConfig.timezone = preferences.getString("timezone", "");
        deviceConfig.latitude = preferences.getDouble("latitude", 0.0);
        deviceConfig.longitude = preferences.getDouble("longitude", 0.0);
    }
    preferences.end();
    return configured && isValidConfig(deviceConfig);
}

bool saveDeviceConfig(const DeviceConfig &config)
{
    Preferences preferences;
    if (!preferences.begin(CONFIG_NAMESPACE, false)) return false;

    preferences.putString("ssid", config.ssid);
    preferences.putString("password", config.password);
    preferences.putString("timezone", config.timezone);
    preferences.putDouble("latitude", config.latitude);
    preferences.putDouble("longitude", config.longitude);
    preferences.putBool("configured", true);
    preferences.end();
    return true;
}

bool isValidConfig(const DeviceConfig &config)
{
    const size_t passwordLength = config.password.length();
    return config.ssid.length() > 0 && config.ssid.length() <= 32 &&
           (passwordLength == 0 || (passwordLength >= 8 && passwordLength <= 63)) &&
           config.timezone.length() > 0 && config.timezone.length() <= 63 &&
           isfinite(config.latitude) && config.latitude >= -90.0 && config.latitude <= 90.0 &&
           isfinite(config.longitude) && config.longitude >= -180.0 && config.longitude <= 180.0;
}

String htmlEscape(const String &value)
{
    String escaped;
    escaped.reserve(value.length());
    for (size_t i = 0; i < value.length(); i++) {
        switch (value[i]) {
        case '&': escaped += "&amp;"; break;
        case '"': escaped += "&quot;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        default: escaped += value[i]; break;
        }
    }
    return escaped;
}

String configurationPage(const String &message)
{
    String page = "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                  "<title>Moon Phase Clock Setup</title><style>body{font:16px sans-serif;max-width:32rem;margin:2rem auto;padding:0 1rem;background:#101820;color:#f2f4f3}"
                  "label{display:block;margin:1rem 0 .35rem}input{box-sizing:border-box;width:100%;padding:.7rem;font:inherit}button{margin-top:1.2rem;padding:.75rem 1rem;font:inherit}small{color:#c4cfce}</style></head><body>"
                  "<h1>Moon Phase Clock</h1><h2>Wi-Fi setup</h2>";
    if (message.length()) page += "<p>" + htmlEscape(message) + "</p>";
    page += "<form method=\"post\" action=\"/save\">"
            "<label for=\"ssid\">Wi-Fi network name</label><input id=\"ssid\" name=\"ssid\" maxlength=\"32\" required value=\"" + htmlEscape(deviceConfig.ssid) + "\">"
            "<label for=\"password\">Wi-Fi password</label><input id=\"password\" name=\"password\" type=\"password\" maxlength=\"63\" autocomplete=\"new-password\">"
            "<small>Leave blank only if the network is open.</small>"
            "<label for=\"latitude\">Latitude</label><input id=\"latitude\" name=\"latitude\" type=\"number\" step=\"any\" min=\"-90\" max=\"90\" required value=\"" + String(deviceConfig.latitude, 6) + "\">"
            "<label for=\"longitude\">Longitude</label><input id=\"longitude\" name=\"longitude\" type=\"number\" step=\"any\" min=\"-180\" max=\"180\" required value=\"" + String(deviceConfig.longitude, 6) + "\">"
            "<label for=\"timezone\">POSIX time-zone rule</label><input id=\"timezone\" name=\"timezone\" maxlength=\"63\" required value=\"" + htmlEscape(deviceConfig.timezone) + "\">"
            "<small>Example: PST8PDT,M3.2.0,M11.1.0</small><br><button type=\"submit\">Save and connect</button></form></body></html>";
    return page;
}

void handleConfigurationSave()
{
    DeviceConfig candidate;
    candidate.ssid = configServer.arg("ssid");
    candidate.password = configServer.arg("password");
    candidate.timezone = configServer.arg("timezone");

    String latitudeValue = configServer.arg("latitude");
    String longitudeValue = configServer.arg("longitude");
    char *latitudeEnd = nullptr;
    char *longitudeEnd = nullptr;
    candidate.latitude = strtod(latitudeValue.c_str(), &latitudeEnd);
    candidate.longitude = strtod(longitudeValue.c_str(), &longitudeEnd);

    if (latitudeEnd == latitudeValue.c_str() || *latitudeEnd != '\0' ||
        longitudeEnd == longitudeValue.c_str() || *longitudeEnd != '\0' || !isValidConfig(candidate)) {
        configServer.send(400, "text/html; charset=utf-8", configurationPage("Please check the network, coordinates, and time-zone rule."));
        return;
    }
    if (!saveDeviceConfig(candidate)) {
        configServer.send(500, "text/html; charset=utf-8", configurationPage("Could not save settings. Please try again."));
        return;
    }

    configServer.send(200, "text/html; charset=utf-8", "<p>Settings saved. The clock is restarting and will connect to Wi-Fi.</p>");
    delay(750);
    ESP.restart();
}

void startProvisioningPortal()
{
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_AP);

    String mac = WiFi.softAPmacAddress();
    mac.replace(":", "");
    String apName = "MoonPhase-" + mac.substring(mac.length() - 4);

    if (!WiFi.softAP(apName.c_str())) {
        Serial.println("Failed to start setup access point.");
        while (true) delay(1000);
    }

    IPAddress portalIp = WiFi.softAPIP();
    dnsServer.start(DNS_PORT, "*", portalIp);
    configServer.on("/", HTTP_GET, []() {
        configServer.send(200, "text/html; charset=utf-8", configurationPage(""));
    });
    configServer.on("/save", HTTP_POST, handleConfigurationSave);
    configServer.onNotFound([]() {
        configServer.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
        configServer.send(302, "text/plain", "");
    });
    configServer.begin();

    Serial.println("Wi-Fi setup required.");
    Serial.print("Connect to access point: ");
    Serial.println(apName);
    Serial.println("Setup access point is open; no password is required.");
    Serial.print("Open: http://");
    Serial.println(portalIp);

    while (true) {
        dnsServer.processNextRequest();
        configServer.handleClient();
        delay(2);
    }
}

bool connectToWiFi()
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(deviceConfig.ssid.c_str(), deviceConfig.password.c_str());
    Serial.print("Connecting to WiFi");
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
        delay(500);
        Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println(" connected.");
        return true;
    } else {
        Serial.println(" failed to connect within 20s.");
        return false;
    }
}

bool waitForTimeSync(uint32_t timeoutMs)
{
    Serial.print("Waiting for NTP time sync");
    uint32_t start = millis();
    time_t now;
    struct tm timeinfo;

    while (millis() - start < timeoutMs) {
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year >= (2023 - 1900)) {
            Serial.println(" synced.");
            return true;
        }
        delay(500);
        Serial.print(".");
    }
    Serial.println(" timed out -- will keep retrying in the background.");
    return false;
}

void updateClockLabels()
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_year < (2023 - 1900)) {
        // Time not synced yet
        return;
    }

    char timeStr[9]; // "HH:MM:SS"
    int displayHour = timeinfo.tm_hour % 12;
    if (displayHour == 0) displayHour = 12;
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", displayHour, timeinfo.tm_min, timeinfo.tm_sec);

    char dateStr[11]; // "13 Aug 26"
    strftime(dateStr, sizeof(dateStr), "%d %b %y", &timeinfo);

    lv_label_set_text(ui_time, timeStr);
    lv_label_set_text(ui_time_period, timeinfo.tm_hour < 12 ? "am" : "pm");
    lv_obj_align_to(ui_time_period, ui_time, LV_ALIGN_OUT_RIGHT_BOTTOM, 2, -2);
    lv_label_set_text(ui_date, dateStr);
    lv_obj_invalidate(ui_time);
    lv_obj_invalidate(ui_time_period);
    lv_obj_invalidate(ui_date);
    lv_refr_now(NULL);
}

void setMoonImage(int index)
{
    if (index < 0) index = 0;
    if (index > 29) index = 29;
    lv_img_set_src(ui_img_moon, moon_frames[index]);
    lv_obj_invalidate(ui_img_moon);
    lv_refr_now(NULL); 
}

void playMoonIntroAnimation()
{
    const uint16_t frameDelayMs = 100; 
    for (int i = 0; i < 30; i++) {
        setMoonImage(i);
        delay(frameDelayMs);
    }
}

void updateMoonData()
{
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected. Skipping moon data fetch.");
        return;
    }

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_year < (2023 - 1900)) {
        Serial.println("Time not set yet. Skipping moon data fetch.");
        return;
    }

    char dateStr[25];
    if (strftime(dateStr, sizeof(dateStr), "%Y-%m-%dT%H:%M", &timeinfo) == 0) {
        Serial.println("Failed to format date string.");
        return;
    }

    String url = "https://svs.gsfc.nasa.gov/api/dialamoon/";
    url += dateStr;

    Serial.print("Fetching moon data: ");
    Serial.println(url);

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.begin(client, url);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();

        DynamicJsonDocument doc(2048);
        DeserializationError err = deserializeJson(doc, payload);
        if (err) {
            Serial.print("JSON parse failed: ");
            Serial.println(err.c_str());
            http.end();
            return;
        }

        double age = doc["age"].as<double>();
        Serial.printf("Moon age: %.3f days\n", age);

        String phaseName = getMoonPhase(age);
        lv_label_set_text(ui_phase, phaseName.c_str());
        lv_obj_invalidate(ui_phase);
        lv_refr_now(NULL);

        setMoonImage(getMoonImageIndex(age));
        moonDataValid = true;

    } else {
        Serial.printf("Failed to fetch moon data, HTTP code: %d\n", httpCode);
    }

    http.end();
}

String getMoonPhase(double age)
{
    if (age < 1.84566) return "New Moon";
    else if (age < 5.53699) return "Waxing Crescent";
    else if (age < 9.22831) return "First Quarter";
    else if (age < 12.91963) return "Waxing Gibbous";
    else if (age < 16.61096) return "Full Moon";
    else if (age < 20.30228) return "Waning Gibbous";
    else if (age < 23.99361) return "Last Quarter";
    else return "Waning Crescent";
}

int getMoonImageIndex(double age)
{

    int idx = (int)(age / 29.53 * 30);
    if (idx > 29) idx = 29;
    if (idx < 0) idx = 0;
    return idx;
}

// Fetches today's moonrise/moonset for the configured location from MET Norway's free
// Sunrise/Moon API (no API key needed) and updates ui_moon_times.
// Docs: https://api.met.no/weatherapi/sunrise/3.0/documentation
void updateMoonRiseSet()
{
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected. Skipping moonrise/set fetch.");
        return;
    }

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_year < (2023 - 1900)) {
        Serial.println("Time not set yet. Skipping moonrise/set fetch.");
        return;
    }

    char dateStr[11]; // "YYYY-MM-DD"
    strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &timeinfo);

    // Build the "+HH:MM" / "-HH:MM" UTC offset the API wants. We compute
    // this ourselves (rather than relying on the BSD tm_gmtoff extension,
    // which isn't available on every ESP32 core/toolchain combo) by
    // comparing local wall-clock time against UTC for the same instant.
    struct tm utcTimeinfo;
    gmtime_r(&now, &utcTimeinfo);
    struct tm localCopy = timeinfo;
    localCopy.tm_isdst = 0;
    utcTimeinfo.tm_isdst = 0;
    long gmtOffset = (long)difftime(mktime(&localCopy), mktime(&utcTimeinfo));

    char offsetStr[8];
    snprintf(offsetStr, sizeof(offsetStr), "%c%02ld:%02ld",
             gmtOffset < 0 ? '-' : '+',
             labs(gmtOffset) / 3600,
             (labs(gmtOffset) % 3600) / 60);

    String url = "https://api.met.no/weatherapi/sunrise/3.0/moon?lat=";
    url += String(deviceConfig.latitude, 4);
    url += "&lon=";
    url += String(deviceConfig.longitude, 4);
    url += "&date=";
    url += dateStr;
    url += "&offset=";
    url += offsetStr;

    Serial.print("Fetching moonrise/set: ");
    Serial.println(url);

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.begin(client, url);
    // MET Norway's terms of service ask every client to identify itself --
    // put your own app name / a contact address here.
    // See https://api.met.no/doc/TermsOfService
    http.addHeader("User-Agent", "MoonPhaseClock/1.0 (your-email@example.com)");

    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();

        DynamicJsonDocument doc(2048);
        DeserializationError err = deserializeJson(doc, payload);
        if (err) {
            Serial.print("Moonrise/set JSON parse failed: ");
            Serial.println(err.c_str());
            http.end();
            return;
        }

        const char *riseTime = doc["properties"]["moonrise"]["time"] | (const char *)nullptr;
        const char *setTime  = doc["properties"]["moonset"]["time"]  | (const char *)nullptr;

        // Times look like "2026-09-07T06:15+10:00" -- since we asked for
        // the local offset, the HH:MM we want sits right after the 'T'.
        char riseStr[6] = "--:--";
        char setStr[6]  = "--:--";
        if (riseTime && strlen(riseTime) >= 16) {
            memcpy(riseStr, riseTime + 11, 5);
            riseStr[5] = '\0';
        }
        if (setTime && strlen(setTime) >= 16) {
            memcpy(setStr, setTime + 11, 5);
            setStr[5] = '\0';
        }
        // Note: moonrise or moonset can legitimately be missing/null for a
        // given local day (it happens roughly once a fortnight) -- that's
        // when you'll see "--:--" rather than a fetch error.

        char line[40];
        snprintf(line, sizeof(line), "^ %s\nv %s", riseStr, setStr);
        lv_label_set_text(ui_moon_times, line);
        lv_obj_invalidate(ui_moon_times);
        lv_refr_now(NULL);

    } else {
        Serial.printf("Failed to fetch moonrise/set, HTTP code: %d\n", httpCode);
    }

    http.end();
}
