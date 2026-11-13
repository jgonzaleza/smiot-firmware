#include "firmware_context.h"
#include "logger.h"
#include "app_utils.h"
#include "network.h"
#include "sensors.h"
#include "gps_module.h"
#include "power_policy.h"
#include "queue_manager.h"
#include "time_manager.h"
#include "ota_guard.h"
#include "csv_parse.h"
#include "network_policy.h"
#include "variant.h"
#include "wake_policy.h"
#include "daily_log.h"
#include <string.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <ModbusMaster.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_task_wdt.h>

static const uint8_t CELL_RETRY_EVERY = 6;

static RotationState rotation_state() {
    RotationState state;
    memset(&state, 0, sizeof(state));
    state.index = current_network_index;
    state.failures = consecutive_network_changes;
    state.max_failures = MAX_NETWORK_FAILURES;
    state.slot_count = 4;
    state.local_sim = (global_apn == "local_carrier");
    state.sticky_since = net_sticky_since;
    return state;
}

static void store_rotation(const RotationState& state) {
    current_network_index = state.index;
    consecutive_network_changes = state.failures;
    if (state.sticky_since == net_sticky_since) return;
    net_sticky_since = state.sticky_since;
    preferences.begin("devcfg", false);
    preferences.putUInt("nstk", net_sticky_since);
    preferences.end();
}

void note_uplink_success() {
    RotationState state = rotation_state();
    rotation_on_success(&state, (uint32_t)get_current_timestamp());
    store_rotation(state);
}

void note_uplink_failure() {
    RotationState state = rotation_state();
    rotation_on_failure(&state);
    store_rotation(state);
}

static void capture_registered_plmn() {
    // Ask for the numeric operator, then read it. Format 2 does not change the selected network.
    modem.sendAT(GF("+COPS=3,2"));
    modem.waitResponse(3000);
    String response;
    modem.sendAT(GF("+COPS?"));
    if (modem.waitResponse(5000L, response) != 1) return;
    char plmn[8];
    if (!cops_numeric_plmn(response.c_str(), plmn, sizeof(plmn))) return;
    registered_plmn = plmn;
    local_logger("Registered PLMN: ", registered_plmn);
}

// 1 ready, 2 locked, 0 not ready or no answer.
// getSimStatus() treats "NOT READY" as READY because endsWith("READY") wins.
static int modem_cpin_state() {
    String response;
    modem.sendAT(GF("+CPIN?"));
    modem.waitResponse(1000, response);
    const char* text = response.c_str();
    if (cpin_text_is_ready(text)) return 1;
    if (wake_text_find(text, "SIM PIN") || wake_text_find(text, "SIM PUK")) return 2;
    return 0;
}

// v3.0.0 go_sleep (endnode/endnode.ino, tag v3.0.0). Stop when AT+CPOF
// is accepted, wait 5s, then wait until AT stops answering.
void power_off_modem_for_sleep() {
    int i = MODEM_CPOF_TRIES;
    while (!modem.poweroff() && i > 0) {
        feed_watchdog();
        i--;
        delay(MODEM_CPOF_GAP_MS);
    }
    delay(MODEM_CPOF_SETTLE_MS);
    int k = MODEM_CPOF_AT_TRIES;
    while (modem.testAT() && k > 0) {
        feed_watchdog();
        delay(1000);
        k--;
    }
}

void init_modem() {
    Serial.begin(115200); // Set console baud rate

    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);

    gpio_hold_dis((gpio_num_t)PIN_5V_EN);
    gpio_hold_dis((gpio_num_t)RS485_EN_PIN);
    gpio_hold_dis((gpio_num_t)RESET_RAIN_SENSOR_PIN);
    gpio_hold_dis((gpio_num_t)BOARD_SD_CS_PIN);
    gpio_deep_sleep_hold_dis();

#ifdef BOARD_POWERON_PIN
    pinMode(BOARD_POWERON_PIN, OUTPUT);
    digitalWrite(BOARD_POWERON_PIN, HIGH);
#endif

#ifdef MODEM_RESET_PIN
    // v3.0.0: release the sleep hold, then reset. The module starts
    // during the reset pulse. hold_dis comes before pinMode, as in v3.
    gpio_hold_dis((gpio_num_t)MODEM_RESET_PIN);
    pinMode(MODEM_RESET_PIN, OUTPUT);
    digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL); delay(100);
    digitalWrite(MODEM_RESET_PIN, MODEM_RESET_LEVEL); delay(2600);
    digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);
#endif

    pinMode(MODEM_DTR_PIN, OUTPUT);
    digitalWrite(MODEM_DTR_PIN, HIGH);

    pinMode(BOARD_PWRKEY_PIN, OUTPUT);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);
    delay(100);
    digitalWrite(BOARD_PWRKEY_PIN, HIGH);
    delay(MODEM_PWRKEY_ON_MS);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);

    delay(1000);
    int retry = 0;
    bool at_ok = false;
    int k = MODEM_AT_PROBES;
    while (!(at_ok = modem.testAT(1000)) && k > 0) {
        feed_watchdog();
        k--;
        if (v300_pwrkey_retry_due(retry)) {
            digitalWrite(BOARD_PWRKEY_PIN, LOW);
            delay(100);
            digitalWrite(BOARD_PWRKEY_PIN, HIGH);
            delay(MODEM_PWRKEY_RETRY_MS);
            digitalWrite(BOARD_PWRKEY_PIN, LOW);
            retry = 0;
        }
    }
    daily_log_net_at(at_ok ? 1 : 0);
    if (!at_ok) daily_log_net_reason("at_fail");
    modem_was_powered = true;

    //Put the Led Pin as Output
    pinMode(LED_RED_PIN, OUTPUT);
    pinMode(LED_GREEN_PIN, OUTPUT);
    pinMode(LED_BLUE_PIN, OUTPUT);

    pinMode(RESET_RAIN_SENSOR_PIN, OUTPUT);
    digitalWrite(RESET_RAIN_SENSOR_PIN, LOW);   

    
    #ifdef DEBUG
        Serial.println("Modem is online!");
        Serial.println("#### Wakeup Completed ####");
    #endif
}

static bool net_deadline(unsigned long phase_start) {
    if (!phase_deadline_hit(phase_start, NET_PHASE_BUDGET_MS)) return false;
    daily_log_net_reason("deadline");
    local_logger("init_network deadline");
    return true;
}

bool init_network(NetworkMode mode_setup_value, bool show_status) {
    unsigned long phase_start = millis();
    // Check if SIM card is online
    int cpin = 0;
    bool result = false;
    int k = 30;
    while (cpin != 1 && k > 0) {
        if (net_deadline(phase_start)) {
            daily_log_net_sim(0);
            return false;
        }
        k--;
        cpin = modem_cpin_state();
        if (cpin == 1) {
            #ifdef DEBUG
                Serial.println("SIM card online");
            #endif
            break;
        }
        if (cpin == 2) {
            Serial.println("The SIM card is locked. Please unlock the SIM card first.");
        }
        if (show_status) {
            //The blink gives us an one second delay
            blink_led(0, 255, 0);
        } else {
            delay(1000);
        }
    }

    if (cpin != 1) {
        daily_log_net_sim(0);
        daily_log_net_reason(cpin == 2 ? "sim_locked" : "sim_timeout");
        if (show_status) {
            blink_led(255, 0, 0, 10000);
        }
        return result;
    }
    daily_log_net_sim(1);

    if (!modem.setNetworkMode(mode_setup_value)) {
        local_logger("Set network mode failed!");
    }
   
    String at_response;
    // Backend APN, then ICCID prefix, then DEFAULT_NETWORK_APN. local only if the backend says so.
    #ifdef DEFAULT_NETWORK_APN
        local_logger("Detecting SIM type via ICCID...");
        String iccid = cell_client.getSimCCID();
        const char* backend_apn = configured_apn_set ? configured_apn.c_str() : NULL;
        ApnChoice choice = choose_apn(iccid.c_str(), backend_apn, SIM_PREFIXES, DEFAULT_NETWORK_APN);
        sim_iccid = choice.iccid;
        sim_label = choice.sim;
        if (choice.iccid[0]) {
            local_logger("ICCID: ", choice.iccid);
            local_logger("SIM Detected: ", sim_log_name(choice.sim));
        } else {
            local_logger("Error: Failed to read ICCID or SIM missing.");
            if (show_status) blink_led(255, 0, 0, 10000);
        }
        if (choice.unknown_iccid) {
            if (!sim_unknown_reported) {
                local_logger("SIM prefix is unknown. Using the default APN.");
                add_sensor_error("[SIM_UNKNOWN_ICCID]");
                sim_unknown_reported = true;
            }
        } else {
            sim_unknown_reported = false;
        }
        if (choice.set_apn) {
            global_apn = choice.apn;
            local_logger("Setting network APN to: ", global_apn);
            modem.sendAT(GF("+CGDCONT=1,\"IP\",\""), global_apn, "\"");
            if (modem.waitResponse() != 1) {
                local_logger("Error: Failed to set APN context!");
                if (show_status) blink_led(255, 0, 0, 10000);
            }
        } else {
            global_apn = "local_carrier";
            local_logger("Skipping manual APN configuration.");
        }
    #endif

    // Carrier rotation. Automatic is index 0. A backend "local" SIM stays on +COPS=0.
    RotationState rotation = rotation_state();
    int rotation_before = rotation.index;
    uint32_t sticky_sec = (uint32_t)NET_STICKY_HOURS * 3600UL;
    rotation_prepare(&rotation, (uint32_t)get_current_timestamp(), sticky_sec);
    if (rotation.index != rotation_before) {
        local_logger("Max failures reached or sticky carrier expired. Switching network to index: ", rotation.index);
    }
    store_rotation(rotation);

    String forced_plmn = "";
    if (!rotation.local_sim && rotation.index > 0 && rotation.index < 4) {
        forced_plmn = plmn_table[rotation.index];
        forced_plmn.trim();
    }
    if (rotation.local_sim || rotation.index == 0 || forced_plmn.length() == 0 || forced_plmn == "AUTO") {
        local_logger("Network set to automatic (+COPS=0)");
        modem.sendAT(GF("+COPS=0"));
        modem.waitResponse(10000L, at_response);
        local_logger("Automatic registration: ", at_response);
    } else {
        local_logger("Forcing manual network (+COPS=1,2): ", forced_plmn);
        // Deregister first so a stuck cell is not reused.
        modem.sendAT(GF("+COPS=2"));
        modem.waitResponse(5000L, at_response);
        local_logger("Forced deregister: ", at_response);
        // Manual search can take a while. Allow 30 seconds, LTE (access technology 7) first.
        modem.sendAT(GF("+COPS=1,2,\""), forced_plmn, GF("\",7"));
        int registration = modem.waitResponse(30000L, at_response);
        local_logger("Manual registration: ", at_response);
        if (registration != 1) {
            local_logger("Manual registration with access technology 7 failed. Retrying the same PLMN without it.");
            modem.sendAT(GF("+COPS=1,2,\""), forced_plmn, GF("\""));
            modem.waitResponse(30000L, at_response);
            local_logger("Manual registration retry: ", at_response);
        }
    }

    // Check network registration status and network signal status
    int16_t sq ;
    #ifdef DEBUG
        Serial.println("Wait for the modem to register with the network.");
    #endif
    RegStatus status = REG_NO_RESULT;
    k = 60;
    while ((status == REG_NO_RESULT || status == REG_SEARCHING || status == REG_UNREGISTERED) && (k > 0)) {
        if (net_deadline(phase_start)) {
            daily_log_net_reg((int)status);
            return false;
        }
        k--;
        status = modem.getRegistrationStatus();
        switch (status) {
            case REG_UNREGISTERED:
                local_logger("REG_UNREGISTERED . . .");
                if (show_status) {
                    blink_led(0, 255, 0);
                } else {
                    delay(1000);
                }
                break;
            case REG_SEARCHING:
                #ifdef DEBUG
                    sq = modem.getSignalQuality();
                    local_logger("Signal Quality: ", sq);
                #endif
                if (show_status) {
                    blink_led(0, 255, 0);
                } else {
                    delay(1000);
                } 
                break;
            case REG_EMERGENCY_ONLY:
                local_logger("Attached for emergency bearer services only");
                if (show_status) {
                    blink_led(0, 255, 0);
                }                
                break;
            case REG_DENIED:
                local_logger("Network registration was rejected, please check if the APN is correct");
                daily_log_net_reg((int)status);
                daily_log_net_reason("reg_denied");
                return false;
            case REG_OK_HOME:
                local_logger("Online registration successful");                
                break;
            case REG_OK_ROAMING:
                local_logger("Network registration successful, currently in roaming mode");
                break;
            default:
                local_logger("Registration Status: ", status);
                if (show_status) {
                    blink_led(0, 255, 0);
                } else {
                    delay(1000);
                }
                break;
        }
    }

    if (status == REG_NO_RESULT || status == REG_SEARCHING || status == REG_UNREGISTERED) {
        local_logger("REGISTRATION FAILED!");
        daily_log_net_reg((int)status);
        daily_log_net_reason("reg_fail");
        if (show_status) {
            blink_led(255, 0, 0, 10000);
        }
        // If registration failed restart the modem and try again later
        modem.restart();
        return result;
    }

    daily_log_net_reg((int)status);
    local_logger("Registration Status: ", status);
    if (status == REG_OK_HOME || status == REG_OK_ROAMING) capture_registered_plmn();
    if (show_status) {
        blink_led(0, 255, 0);
    }                

    //For Roaming Enabling the network din't work. We use wait for network
    bool enable_network = false;
    int netopen_seen = 0;
    String ipAddress = "";
    if (status != REG_OK_ROAMING && status != REG_EMERGENCY_ONLY) {
        k = 60;
        enable_network = modem.enableNetwork();
        netopen_seen = enable_network ? 1 : 0;
        while (!enable_network && k > 0) {
            if (net_deadline(phase_start)) {
                daily_log_net_open(netopen_seen);
                return false;
            }
            enable_network = modem.enableNetwork();
            netopen_seen = enable_network ? 1 : 0;
            local_logger("Trying to enable network: ", k); 
            if (show_status) {
                blink_led(0, 255, 0);
            } 
            k--;
        }
        if (enable_network) {  
            local_logger("GET IP:");
            ipAddress = modem.getLocalIP();
            ipAddress.trim();
            #ifdef DEBUG
                if (ipAddress.isEmpty()) {
                    local_logger("IP EMPTY . . .");                    
                    delay(1000);
                }           
            #endif
        }
        if (show_status) {
            blink_led(0, 255, 0);
        } 
    } else {
        local_logger("TRY TO ENABLE NETWORK: ");
        k = 10;          
        while (ipAddress.isEmpty() && k > 0) {
            if (net_deadline(phase_start)) {
                daily_log_net_open(netopen_seen);
                return false;
            }
            k--;
            #ifdef DEBUG
                Serial.println("");
                Serial.println("################################################################");
                Serial.print("TRY NUMBER: ");
                Serial.println(k);            
            #endif                        
            int p = 5;
            bool enable_network = modem.enableNetwork();
            netopen_seen = enable_network ? 1 : 0;
            while (!enable_network && p > 0) {
                if (net_deadline(phase_start)) {
                    daily_log_net_open(netopen_seen);
                    return false;
                }
                p--;
                #ifdef DEBUG
                    Serial.println("");
                    Serial.println(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>");                
                    Serial.println("");
                    Serial.print("Trying to enable network:");
                    Serial.println(p);
                    Serial.println("");
                #endif                
                enable_network = modem.enableNetwork();
                netopen_seen = enable_network ? 1 : 0;
                Serial.println("");
                delay(1000);                
            }
            if (enable_network) {  
                #ifdef DEBUG
                    Serial.println("");         
                    Serial.println("//////////////////////////////////////////////////////////////////");     
                    Serial.println("GET IP:");
                #endif
                ipAddress = modem.getLocalIP();
                ipAddress.trim();
                #ifdef DEBUG
                    if (ipAddress.isEmpty()) {
                        Serial.println("IP EMPTY . . .");                    
                        delay(1000);
                    }           
                #endif
            }
            if (show_status) {
                blink_led(0, 255, 0);
            }
        }    
    }

    result = !ipAddress.isEmpty();
    daily_log_net_open(netopen_seen);
    daily_log_net_ip(result ? 1 : 0);
    if (result) daily_log_net_clear_reason();
    else daily_log_net_reason(netopen_seen ? "no_ip" : "netopen_fail");

    #ifdef DEBUG
        if (result) {
            modem.sendAT(GF("+CNUM"));
            if (modem.waitResponse(3000, at_response) != 1) {
                Serial.println("Read Phone Number Failed!");        
            }  else {
                Serial.print("RESPONSE CNUM: ");
                at_response.replace("AT+CNUM", "");  
                at_response.replace("+CNUM:", "");            
                at_response.replace(GSM_NL "OK" GSM_NL, "");
                at_response.replace(GSM_NL, "");
                at_response.trim();        
                Serial.println(at_response);        
            }
            Serial.print("Network IP:"); 
            Serial.println(ipAddress);    
            Serial.println("OPERATOR: ");
            Serial.print(modem.getOperator());
            delay(1000);
        } else {
            Serial.println("NETWORK ACTIVATION FAILED!!!");
            delay(5000);
        }
    #endif
    
    if (!result && show_status){
        blink_led(255, 0, 0, 10000);
    }

    int16_t sq_now = modem.getSignalQuality();
    if (sq_now >= 0 && sq_now <= 99) last_rssi = sq_now; 

#ifdef DEBUG_FORCE_ROTATION
    current_network_index++;
    if (current_network_index > 3) current_network_index = 0;
    consecutive_network_changes = 0;
    if (net_sticky_since != 0) {
        net_sticky_since = 0;
        preferences.begin("devcfg", false);
        preferences.putUInt("nstk", 0);
        preferences.end();
    }
    local_logger("DEBUG_FORCE_ROTATION: manual network index is now ", current_network_index);
#endif


    return result;    
}
void use_cell_client() {
    comm.setClient(&cell_client);
    g_rtc.last_transport = 0;
    g_rtc.wakes_since_cell = 0;
    rtc_persist_commit();
}
#ifdef FEATURE_WIFI
bool use_wifi_client() {
    feed_watchdog();
    if (!wifi_client.network_connected() && !wifi_client.init_network()) return false;
    comm.setClient(&wifi_client);
    g_rtc.last_transport = 1;
    if (g_rtc.wakes_since_cell < 255) g_rtc.wakes_since_cell++;
    rtc_persist_commit();
    return true;
}
#endif
bool connect_uplink(bool show_status) {
#ifdef FEATURE_WIFI
    bool prefer_wifi = (g_rtc.last_transport == 1 && g_rtc.wakes_since_cell < CELL_RETRY_EVERY);
    if (prefer_wifi && use_wifi_client()) return true;
#endif
    bool ok = false;
    if (daily_log_net_trace()->at == 0) {
        local_logger("Modem did not answer AT. Skipping registration.");
    } else {
        local_logger("Initializing Network Try 1 LTE");
        ok = init_network(MODEM_NETWORK_LTE, show_status);
        feed_watchdog();
        if (!ok && network_mode_retries_allowed(daily_log_net_trace()->sim)) {
            local_logger("Initializing Network Try 2 LTE");
            ok = init_network(MODEM_NETWORK_AUTO, show_status);
            feed_watchdog();
            if (!ok && network_mode_retries_allowed(daily_log_net_trace()->sim)) {
                local_logger("Initializing Network Try 1 GSM");
                ok = init_network(MODEM_NETWORK_GSM, show_status);
                feed_watchdog();
            }
        } else if (!ok) {
            local_logger("SIM did not answer. Skipping further registration attempts.");
        }
    }
    if (ok) {
        use_cell_client();
        return true;
    }
#ifdef FEATURE_WIFI
    if (use_wifi_client()) return true;
#endif
    comm.setClient(&cell_client);
    return false;
}
