#pragma once

#include "ICommunicationClient.h"

class CommunicationManager
{
public:
    CommunicationManager();
    void setClient(ICommunicationClient* client);
    bool hasClient();

    bool init_network();
    bool network_connected();
    void network_disconnect();
    void disableNetwork();
    SimStatus getSimStatus();
    bool setNetworkMode(NetworkMode mode);
    String getSimCCID();
    RegStatus getRegistrationStatus();
    bool enableNetwork();
    String getLocalIP();
    void gprsDisconnect();
    bool isCellular();
    bool isWifi();

    bool init_mqtt_connection();
    bool mqtt_connect(uint8_t clientIndex, const char *server, uint16_t port, const char *clientID,
        const char *username = NULL, const char *password = NULL, uint32_t keepalive_time = 60);
    void mqtt_disconnect(uint8_t client_index);
    bool mqtt_connected();
    bool mqtt_end();
    bool mqtt_publish(int client_index, const char* topic, const char* payload);
    bool mqtt_subscribe(int client_index, const char* topic);
    void mqtt_set_callback(void (*callback)(const char* topic, const uint8_t* payload, uint32_t len));
    void mqtt_begin(bool use_ssl);
    void mqtt_set_certificate(const char* root_ca, const char* client_certificate, const char* client_private_key);
    bool mqtt_handle(uint32_t timeout = 100);

    bool https_begin();
    void https_end();
    bool https_set_url(const String& url);
    bool https_add_header(const char* key, const char* value);
    int https_get();
    size_t https_get_size();
    int https_body(uint8_t* buffer, size_t length);
    int https_post(const String& payload);
    String https_body();
    int getSignalQuality();

private:
    ICommunicationClient* client;
};
