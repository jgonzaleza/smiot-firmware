#pragma once

#include <Arduino.h>
#include <TinyGsmClient.h>

class ICommunicationClient
{
public:
    virtual ~ICommunicationClient() {}

    virtual bool init_network() = 0;
    virtual bool network_connected() = 0;
    virtual void network_disconnect() = 0;
    virtual bool isCellular() = 0;
    virtual bool isWifi() = 0;
    virtual void disableNetwork() = 0;
    virtual SimStatus getSimStatus() = 0;
    virtual bool setNetworkMode(NetworkMode mode) = 0;
    virtual String getSimCCID() = 0;
    virtual RegStatus getRegistrationStatus() = 0;
    virtual bool enableNetwork() = 0;
    virtual String getLocalIP() = 0;
    virtual void gprsDisconnect() = 0;

    virtual bool init_mqtt_connection() = 0;
    virtual bool mqtt_connect(uint8_t clientIndex,
        const char *server, uint16_t port,
        const char *clientID,
        const char *username = NULL,
        const char *password = NULL,
        uint32_t keepalive_time = 60) = 0;
    virtual void mqtt_disconnect(uint8_t client_index) = 0;
    virtual bool mqtt_end() = 0;
    virtual bool mqtt_connected() = 0;
    virtual bool mqtt_publish(int client_index, const char *topic, const char *payload) = 0;
    virtual bool mqtt_subscribe(int client_index, const char *topic) = 0;
    virtual void mqtt_set_callback(void (*callback)(const char *topic, const uint8_t *payload, uint32_t len)) = 0;
    virtual void mqtt_begin(bool use_ssl) = 0;
    virtual void mqtt_set_certificate(const char *root_ca, const char *client_certificate, const char *client_private_key) = 0;
    virtual bool mqtt_handle(uint32_t timeout = 100) = 0;

    virtual bool https_begin() = 0;
    virtual void https_end() = 0;
    virtual bool https_set_url(const String& url) = 0;
    virtual bool https_add_header(const char* key, const char* value) = 0;
    virtual int https_get() = 0;
    virtual size_t https_get_size() = 0;
    virtual int https_body(uint8_t* buffer, size_t length) = 0;
    virtual int https_post(const String& payload) = 0;
    virtual String https_body() = 0;

    virtual int getSignalQuality() = 0;
};
