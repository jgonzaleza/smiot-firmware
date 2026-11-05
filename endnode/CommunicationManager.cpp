#include "CommunicationManager.h"
#include "logger.h"

static bool header_is_secret(const char* key) {
    if (!key) return true;
    String name(key);
    name.toLowerCase();
    return name.indexOf("key") >= 0 || name.indexOf("auth") >= 0 || name.indexOf("token") >= 0;
}

CommunicationManager::CommunicationManager() : client(nullptr) {}

void CommunicationManager::setClient(ICommunicationClient* c) { client = c; }
bool CommunicationManager::hasClient() { return client != nullptr; }

bool CommunicationManager::init_network() { return hasClient() && client->init_network(); }
bool CommunicationManager::network_connected() { return hasClient() && client->network_connected(); }
void CommunicationManager::network_disconnect() { if (hasClient()) client->network_disconnect(); }
void CommunicationManager::disableNetwork() { if (hasClient()) client->disableNetwork(); }
SimStatus CommunicationManager::getSimStatus() { return hasClient() ? client->getSimStatus() : SIM_ERROR; }
bool CommunicationManager::setNetworkMode(NetworkMode mode) { return hasClient() && client->setNetworkMode(mode); }
String CommunicationManager::getSimCCID() { return hasClient() ? client->getSimCCID() : String(""); }
RegStatus CommunicationManager::getRegistrationStatus() { return hasClient() ? client->getRegistrationStatus() : REG_NO_RESULT; }
bool CommunicationManager::enableNetwork() { return hasClient() && client->enableNetwork(); }
String CommunicationManager::getLocalIP() { return hasClient() ? client->getLocalIP() : String(""); }
void CommunicationManager::gprsDisconnect() { if (hasClient()) client->gprsDisconnect(); }
bool CommunicationManager::isCellular() { return hasClient() && client->isCellular(); }
bool CommunicationManager::isWifi() { return hasClient() && client->isWifi(); }

bool CommunicationManager::init_mqtt_connection() { return hasClient() && client->init_mqtt_connection(); }
bool CommunicationManager::mqtt_connect(uint8_t clientIndex, const char *server, uint16_t port, const char *clientID,
    const char *username, const char *password, uint32_t keepalive_time) {
    return hasClient() && client->mqtt_connect(clientIndex, server, port, clientID, username, password, keepalive_time);
}
void CommunicationManager::mqtt_disconnect(uint8_t client_index) { if (hasClient()) client->mqtt_disconnect(client_index); }
bool CommunicationManager::mqtt_connected() { return hasClient() && client->mqtt_connected(); }
bool CommunicationManager::mqtt_end() { return hasClient() && client->mqtt_end(); }
bool CommunicationManager::mqtt_publish(int client_index, const char* topic, const char* payload) {
    return hasClient() && client->mqtt_publish(client_index, topic, payload);
}
bool CommunicationManager::mqtt_subscribe(int client_index, const char* topic) {
    return hasClient() && client->mqtt_subscribe(client_index, topic);
}
void CommunicationManager::mqtt_set_callback(void (*callback)(const char*, const uint8_t*, uint32_t)) {
    if (hasClient()) client->mqtt_set_callback(callback);
}
void CommunicationManager::mqtt_begin(bool use_ssl) { if (hasClient()) client->mqtt_begin(use_ssl); }
void CommunicationManager::mqtt_set_certificate(const char* root_ca, const char* client_certificate, const char* client_private_key) {
    if (hasClient()) client->mqtt_set_certificate(root_ca, client_certificate, client_private_key);
}
bool CommunicationManager::mqtt_handle(uint32_t timeout) { return hasClient() && client->mqtt_handle(timeout); }

bool CommunicationManager::https_begin() { return hasClient() && client->https_begin(); }
void CommunicationManager::https_end() { if (hasClient()) client->https_end(); }
bool CommunicationManager::https_set_url(const String& url) { return hasClient() && client->https_set_url(url); }
bool CommunicationManager::https_add_header(const char* key, const char* value) {
    if (!hasClient()) return false;
    if (header_is_secret(key)) local_logger("HTTPS header set:", key);
    else local_logger("HTTPS header set:", key, value);
    return client->https_add_header(key, value);
}
int CommunicationManager::https_get() { return hasClient() ? client->https_get() : -1; }
size_t CommunicationManager::https_get_size() { return hasClient() ? client->https_get_size() : 0; }
int CommunicationManager::https_body(uint8_t* buffer, size_t length) {
    return hasClient() ? client->https_body(buffer, length) : 0;
}
int CommunicationManager::https_post(const String& payload) { return hasClient() ? client->https_post(payload) : -1; }
String CommunicationManager::https_body() { return hasClient() ? client->https_body() : String(""); }
int CommunicationManager::getSignalQuality() { return hasClient() ? client->getSignalQuality() : 0; }
