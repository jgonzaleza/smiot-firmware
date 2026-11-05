#pragma once

#include "ICommunicationClient.h"

// Included only by the Wi-Fi env. Pulls WiFi, HTTPClient and PubSubClient.
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <PubSubClient.h>

class WifiCommunicationClient : public ICommunicationClient {
public:
    WifiCommunicationClient();
    bool init_network() override;
    bool network_connected() override;
    void network_disconnect() override;
    void disableNetwork() override;
    SimStatus getSimStatus() override;
    bool setNetworkMode(NetworkMode mode) override;
    String getSimCCID() override;
    RegStatus getRegistrationStatus() override;
    bool enableNetwork() override;
    String getLocalIP() override;
    void gprsDisconnect() override;
    bool isWifi() override;
    bool isCellular() override;

    bool init_mqtt_connection() override;
    bool mqtt_connect(uint8_t clientIndex, const char *server, uint16_t port, const char *clientID,
        const char *username = NULL, const char *password = NULL, uint32_t keepalive_time = 60) override;
    void mqtt_disconnect(uint8_t client_index) override;
    bool mqtt_end() override;
    bool mqtt_connected() override;
    bool mqtt_publish(int client_index, const char* topic, const char* payload) override;
    bool mqtt_subscribe(int client_index, const char* topic) override;
    void mqtt_set_callback(void (*callback)(const char*, const uint8_t*, uint32_t)) override;
    void mqtt_begin(bool use_ssl) override;
    void mqtt_set_certificate(const char* root_ca, const char* client_certificate, const char* client_private_key) override;
    bool mqtt_handle(uint32_t timeout = 100) override;

    bool https_begin() override;
    void https_end() override;
    bool https_set_url(const String& url) override;
    bool https_add_header(const char* key, const char* value) override;
    int https_get() override;
    size_t https_get_size() override;
    int https_body(uint8_t* buffer, size_t length) override;
    int https_post(const String& payload) override;
    String https_body() override;
    int getSignalQuality() override;

private:
    WiFiClientSecure secureClient;
    HTTPClient httpsClient;
    PubSubClient mqttClient;
    String currentUrl;
    bool httpsInitialized = false;
    bool certificateConfigured = false;
    static constexpr uint32_t WIFI_CONNECTION_TIMEOUT_MS = 15000;
    void configureSecureClient();
    bool ensure_https_open();
};
