#include "CellularCommunicationClient.h"

extern TinyGsm modem;

bool CellularCommunicationClient::init_network() { return modem.isNetworkConnected(); }
SimStatus CellularCommunicationClient::getSimStatus() { return modem.getSimStatus(); }
bool CellularCommunicationClient::setNetworkMode(NetworkMode mode) { return modem.setNetworkMode(mode); }
String CellularCommunicationClient::getSimCCID() { return modem.getSimCCID(); }
RegStatus CellularCommunicationClient::getRegistrationStatus() { return modem.getRegistrationStatus(); }
bool CellularCommunicationClient::enableNetwork() { return modem.enableNetwork(); }
String CellularCommunicationClient::getLocalIP() { return modem.getLocalIP(); }
void CellularCommunicationClient::gprsDisconnect() { modem.gprsDisconnect(); }
bool CellularCommunicationClient::network_connected() { return modem.isNetworkConnected(); }
void CellularCommunicationClient::network_disconnect() { modem.disableNetwork(); }
void CellularCommunicationClient::disableNetwork() { modem.disableNetwork(); }
bool CellularCommunicationClient::isCellular() { return true; }
bool CellularCommunicationClient::isWifi() { return false; }

bool CellularCommunicationClient::init_mqtt_connection() {
    modem.mqtt_begin(true);
    return modem.mqtt_connected();
}
bool CellularCommunicationClient::mqtt_connect(uint8_t clientIndex, const char *server, uint16_t port, const char *clientID,
    const char *username, const char *password, uint32_t keepalive_time) {
    return modem.mqtt_connect(clientIndex, server, port, clientID, username, password, keepalive_time);
}
void CellularCommunicationClient::mqtt_disconnect(uint8_t client_index) { modem.mqtt_disconnect(client_index); }
bool CellularCommunicationClient::mqtt_end() { return modem.mqtt_end(); }
bool CellularCommunicationClient::mqtt_connected() { return modem.mqtt_connected(); }
bool CellularCommunicationClient::mqtt_publish(int client_index, const char *topic, const char *payload) {
    return modem.mqtt_publish(client_index, topic, payload);
}
bool CellularCommunicationClient::mqtt_subscribe(int client_index, const char *topic) {
    return modem.mqtt_subscribe(client_index, topic);
}
void CellularCommunicationClient::mqtt_set_callback(void (*callback)(const char *topic, const uint8_t *payload, uint32_t len)) {
    modem.mqtt_set_callback(callback);
}
void CellularCommunicationClient::mqtt_begin(bool use_ssl) { modem.mqtt_begin(use_ssl); }
void CellularCommunicationClient::mqtt_set_certificate(const char* root_ca, const char* client_certificate, const char* client_private_key) {
    modem.mqtt_set_certificate(root_ca, client_certificate, client_private_key);
}
bool CellularCommunicationClient::mqtt_handle(uint32_t timeout) { return modem.mqtt_handle(timeout); }

bool CellularCommunicationClient::https_begin() {
    // HTTPS uses SSL context 1 with CA authentication. MQTT stays on context 0.
    modem.sendAT(GF("+CSSLCFG=\"sslversion\",1,3"));
    modem.waitResponse();
    modem.sendAT(GF("+CSSLCFG=\"enableSNI\",1,1"));
    modem.waitResponse();
    modem.sendAT(GF("+CSSLCFG=\"cacert\",1,\"ca_cert.pem\""));
    modem.waitResponse();
    modem.sendAT(GF("+CSSLCFG=\"authmode\",1,1"));
    modem.waitResponse();
    bool started = modem.https_begin();
    if (started) modem.https_set_ssl_index(1);
    return started;
}
void CellularCommunicationClient::https_end() { modem.https_end(); }
bool CellularCommunicationClient::https_set_url(const String& url) { return modem.https_set_url(url); }
bool CellularCommunicationClient::https_add_header(const char* key, const char* value) { return modem.https_add_header(key, value); }
int CellularCommunicationClient::https_get() { return modem.https_get(); }
size_t CellularCommunicationClient::https_get_size() { return modem.https_get_size(); }
int CellularCommunicationClient::https_body(uint8_t* buffer, size_t length) { return modem.https_body(buffer, length); }
int CellularCommunicationClient::https_post(const String& payload) { return modem.https_post(payload); }
String CellularCommunicationClient::https_body() { return modem.https_body(); }
int CellularCommunicationClient::getSignalQuality() { return modem.getSignalQuality(); }
