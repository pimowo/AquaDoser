#include "webserver.h"
#include <ESPAsyncWebServer.h>
#include "config.h"

extern AsyncWebServer asyncServer;

void handleRoot(AsyncWebServerRequest *request) {
    String content = getConfigPage();
    request->send(200, "text/html", content);
}

void handleTimeAPI(AsyncWebServerRequest *request) {
    time_t utc = now();
    TimeChangeRule *tcr;
    time_t local = CE.toLocal(utc, &tcr);
    String json = "{";
    json += "\"hour\":" + String(hour(local)) + ",";
    json += "\"minute\":" + String(minute(local)) + ",";
    json += "\"second\":" + String(second(local)) + ",";
    json += "\"day\":" + String(day(local)) + ",";
    json += "\"month\":" + String(month(local)) + ",";
    json += "\"year\":" + String(year(local)) + ",";
    json += "\"isDST\":" + String(tcr->offset == 120 ? "true" : "false") + ",";
    json += "\"tzAbbrev\":\"" + String(tcr->abbrev) + "\"";
    json += "}";
    request->send(200, "application/json", json);
}

void handleSaveMQTT(AsyncWebServerRequest *request) {
    // ...implementacja analogiczna do main.cpp, ale z request-> zamiast server.
}

void handleSavePumps(AsyncWebServerRequest *request) {
    // ...implementacja analogiczna do main.cpp, ale z request-> zamiast server.
}

void setupWebServer() {
    asyncServer.on("/", HTTP_GET, handleRoot);
    asyncServer.on("/api/time", HTTP_GET, handleTimeAPI);
    asyncServer.on("/save-mqtt", HTTP_POST, handleSaveMQTT);
    asyncServer.on("/save-pumps", HTTP_POST, handleSavePumps);
    // ...pozostałe endpointy
}
