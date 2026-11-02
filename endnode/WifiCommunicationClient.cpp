#include "WifiCommunicationClient.h"
#include "logger.h"
#include "app_utils.h"
#include "certs/AmazonRootCA.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

WifiCommunicationClient::WifiCommunicationClient() : mqttClient(secureClient) {}

void WifiCommunicationClient::configureSecureClient() {
    if (certificateConfigured) return;
    secureClient.setCACert(AmazonRootCA);
    certificateConfigured = true;
}

bool WifiCommunicationClient::init_network() {
    if (WiFi.status() == WL_CONNECTED) return true;
    local_logger("Wi-Fi connect");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        feed_watchdog();
        if (millis() - start > WIFI_CONNECTION_TIMEOUT_MS) {
            local_logger("Wi-Fi connection timeout");
            return false;
        }
        delay(500);
    }
    local_logger("Wi-Fi connected", WiFi.localIP().toString().c_str());
    return true;
}

bool WifiCommunicationClient::network_connected() { return WiFi.status() == WL_CONNECTED; }
void WifiCommunicationClient::network_disconnect() { WiFi.disconnect(true); }
void WifiCommunicationClient::disableNetwork() { WiFi.disconnect(true); }
bool WifiCommunicationClient::enableNetwork() { return init_network(); }
void WifiCommunicationClient::gprsDisconnect() { WiFi.disconnect(true); }
String WifiCommunicationClient::getLocalIP() { return WiFi.localIP().toString(); }
int WifiCommunicationClient::getSignalQuality() { return WiFi.RSSI(); }
bool WifiCommunicationClient::isWifi() { return true; }
bool WifiCommunicationClient::isCellular() { return false; }
SimStatus WifiCommunicationClient::getSimStatus() { return SIM_ERROR; }
bool WifiCommunicationClient::setNetworkMode(NetworkMode mode) { (void)mode; return true; }
String WifiCommunicationClient::getSimCCID() { return ""; }
RegStatus WifiCommunicationClient::getRegistrationStatus() { return REG_UNREGISTERED; }

bool WifiCommunicationClient::https_begin() {
    configureSecureClient();
    httpsInitialized = true;
    return true;
}
void WifiCommunicationClient::https_end() {
    if (httpsInitialized) {
        httpsClient.end();
        httpsInitialized = false;
    }
}
bool WifiCommunicationClient::https_set_url(const String& url) {
    currentUrl = url;
    return currentUrl.length() > 0;
}
bool WifiCommunicationClient::ensure_https_open() {
    if (!httpsInitialized || currentUrl.length() == 0) return false;
    if (httpsClient.connected()) return true;
    return httpsClient.begin(secureClient, currentUrl);
}
bool WifiCommunicationClient::https_add_header(const char* key, const char* value) {
    if (!ensure_https_open()) return false;
    httpsClient.addHeader(key, value);
    return true;
}
int WifiCommunicationClient::https_get() {
    if (!ensure_https_open()) return -1;
    return httpsClient.GET();
}
int WifiCommunicationClient::https_post(const String& payload) {
    if (!ensure_https_open()) return -1;
    return httpsClient.POST(payload);
}
String WifiCommunicationClient::https_body() { return httpsClient.getString(); }
size_t WifiCommunicationClient::https_get_size() {
    int sz = httpsClient.getSize();
    if (sz < 0) return 0;
    return (size_t)sz;
}
int WifiCommunicationClient::https_body(uint8_t* buffer, size_t length) {
    // A zero return means "no bytes right now", not end of file. The OTA loop
    // keeps reading until the expected size or its own deadline.
    if (!buffer || length == 0) return 0;
    WiFiClient* stream = httpsClient.getStreamPtr();
    if (stream == nullptr || !stream->available()) return 0;
    return stream->readBytes((char*)buffer, length);
}

void WifiCommunicationClient::mqtt_begin(bool use_ssl) {
    (void)use_ssl;
    mqttClient.setBufferSize(12288);
    mqttClient.setClient(secureClient);
}
void WifiCommunicationClient::mqtt_set_certificate(const char* root_ca, const char* client_certificate, const char* client_private_key) {
    secureClient.setCACert(root_ca);
    secureClient.setCertificate(client_certificate);
    secureClient.setPrivateKey(client_private_key);
    certificateConfigured = true;
}
bool WifiCommunicationClient::mqtt_connect(uint8_t clientIndex, const char *server, uint16_t port, const char *clientID,
    const char *username, const char *password, uint32_t keepalive_time) {
    (void)clientIndex;
    if (!certificateConfigured) return false;
    mqttClient.setServer(server, port);
    mqttClient.setKeepAlive(keepalive_time);
    if (username == NULL) return mqttClient.connect(clientID);
    return mqttClient.connect(clientID, username, password);
}
void WifiCommunicationClient::mqtt_disconnect(uint8_t client_index) { (void)client_index; mqttClient.disconnect(); }
bool WifiCommunicationClient::mqtt_connected() { return mqttClient.connected(); }
bool WifiCommunicationClient::mqtt_publish(int client_index, const char* topic, const char* payload) {
    (void)client_index;
    return mqttClient.publish(topic, payload);
}
bool WifiCommunicationClient::mqtt_subscribe(int client_index, const char* topic) {
    (void)client_index;
    return mqttClient.subscribe(topic);
}
void WifiCommunicationClient::mqtt_set_callback(void (*callback)(const char*, const uint8_t*, uint32_t)) {
    mqttClient.setCallback(callback);
}
bool WifiCommunicationClient::mqtt_handle(uint32_t timeout) {
    unsigned long start = millis();
    while ((millis() - start) < timeout) {
        mqttClient.loop();
        feed_watchdog();
        delay(1);
    }
    return mqttClient.connected();
}
bool WifiCommunicationClient::mqtt_end() {
    mqttClient.disconnect();
    return true;
}
bool WifiCommunicationClient::init_mqtt_connection() {
    if (!network_connected()) return false;
    mqtt_begin(true);
    return true;
}
