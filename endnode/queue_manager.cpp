#include <SPI.h>
#include <SD.h>
#include "queue_manager.h"
#include "app_utils.h" // Para is_sd_ready y las rutas de los archivos
#include "logger.h"    // Para local_logger

// ==============================================================================
// INSTANCIACIÓN DE VARIABLES GLOBALES (Declaradas como extern en el .h)
// ==============================================================================
RTC_DATA_ATTR char rtc_payload_pool[MAX_RTC_BUFFER_SIZE];
RTC_DATA_ATTR int rtc_pool_length = 0;

String current_batch_payloads[MAX_BATCH_SIZE];
int current_batch_count = 0;
bool is_batch_from_rtc = true;
int batch_consumed_bytes = 0; // Rastrea cuántos bytes leer y borrar del origen

extern bool is_sdcard_ok;

// ==============================================================================
// FUNCIONES PRIVADAS (Helpers)
// ==============================================================================

void spillover_rtc_to_sd() {
    if (!ensure_sd_active()) {
        add_sd_error("[SPILLOVER_FAIL: SD_NOT_ACTIVE] ");
        return; // Si la SD no sirve, no podemos derramar.
    }

    try {
        local_logger("DERRAME: Moviendo datos de RTC a SD...");
        File file = SD.open(QUEUE_FILE, FILE_APPEND);
        if (file) {
            file.print(rtc_payload_pool); // Volcamos todo el buffer
            file.close();
            // Vaciamos la RTC
            rtc_pool_length = 0;
            rtc_payload_pool[0] = '\0';
            local_logger("DERRAME: Exitoso. RTC vaciada.");
        } else {
            add_sd_error("[SPILLOVER_FAIL: SD_OPEN] ");
            local_logger("DERRAME: Falló al abrir archivo SD.");
        }
    } catch(const std::exception& e) {
        local_logger("DERRAME: Excepción al escribir en SD.");
        add_sd_error(String("[SPILLOVER_EXC_ERROR: ") + e.what() + "] ");
    }
}

void drop_oldest_rtc_record() {
    if (rtc_pool_length == 0) return;

    // Buscamos el primer salto de línea (fin del mensaje más viejo)
    char* first_newline = strchr(rtc_payload_pool, PAYLOAD_DELIMITER);
    
    if (first_newline) {
        int bytes_to_remove = (first_newline - rtc_payload_pool) + 1;
        int remaining_bytes = rtc_pool_length - bytes_to_remove;
        
        // Desplazamos la memoria hacia la izquierda (sobrescribiendo el viejo)
        memmove(rtc_payload_pool, first_newline + 1, remaining_bytes);
        rtc_pool_length = remaining_bytes;
        rtc_payload_pool[rtc_pool_length] = '\0';
        local_logger("RTC LLENA (SIN SD): Dato más viejo descartado (Ring Buffer).");
    } else {
        // Fallback por si hay un string malformado sin delimitador
        rtc_pool_length = 0; 
        rtc_payload_pool[0] = '\0';
    }
}

// ==============================================================================
// INTERFAZ PÚBLICA DEL GESTOR
// ==============================================================================

void queue_manager_init() {
    // =======================================================
    // 1. INICIALIZACIÓN DE MEMORIA DE EMERGENCIA (RTC)
    // =======================================================
    // Asegurarnos de que el string RTC termina en null si es el primer arranque
    // o si hubo corrupción de memoria
    if (rtc_pool_length < 0 || rtc_pool_length > MAX_RTC_BUFFER_SIZE) {
        rtc_pool_length = 0;
        rtc_payload_pool[0] = '\0';
    }
    current_batch_count = 0;
    batch_consumed_bytes = 0;

    // =======================================================
    // 2. INICIALIZACIÓN DE ALMACENAMIENTO FÍSICO (SD CARD)
    // =======================================================
    try {
        SPI.begin(BOARD_SCK_PIN, BOARD_MISO_PIN, BOARD_MOSI_PIN);
        delay(300); // Wait to ensure SPI is ready before SD init
        // Intentamos montar el sistema de archivos de la tarjeta SD
        if (!SD.begin(BOARD_SD_CS_PIN, SPI)) { 
            local_logger("Fallo crítico al montar SD Card en el arranque.");
            is_sdcard_ok = false; // Nuestra bandera global que gatillará el diagnóstico en el setup()
            
            // Agregamos el error al log RTC que viajará en el próximo payload
            add_sd_error("[QM_INIT: SD_MOUNT_FAIL]"); 
        } else {
            local_logger("SD Card montada e inicializada correctamente.");
            // Verificación de salud rápida: comprobar si el archivo de cola existe
            // Si no existe, lo creamos vacío para evitar errores posteriores de "File not found"
            if (!SD.exists(QUEUE_FILE)) {
                File file = SD.open(QUEUE_FILE, FILE_WRITE);
                if (file) {
                    is_sdcard_ok = true;
                    file.close();
                    local_logger("Archivo QUEUE_FILE base creado.");
                }
            }
        }
    } catch(const std::exception& e) {
        local_logger("Excepción durante la inicialización de SD Card.");
        add_sd_error(String("[QM_INIT_SD_ERROR: ") + e.what() + "] ");
        is_sdcard_ok = false;
    }
}

void queue_payload(String payload) {
    // 1. Limpiar el string y asegurar que termine con el delimitador
    payload.trim();
    payload += PAYLOAD_DELIMITER;
    int payload_len = payload.length();

    // 2. Verificar si cabe en la memoria RTC
    while ((rtc_pool_length + payload_len) >= (MAX_RTC_BUFFER_SIZE - 1)) {
        if (ensure_sd_active()) {
            spillover_rtc_to_sd(); // Vaciamos la RTC a la SD
            break; // Ya hay espacio
        } else {
            add_sd_error("[QP_FAIL: SD_NOT_ACTIVE]");
            drop_oldest_rtc_record(); // Borramos el más viejo para hacer campo
        }
    }

    // 3. Insertar el nuevo payload al final del pool de la RTC
    strcpy(rtc_payload_pool + rtc_pool_length, payload.c_str());
    rtc_pool_length += payload_len;
    
    #ifdef DEBUG
    local_logger("Guardado en RTC. Uso actual: ", rtc_pool_length, "/", MAX_RTC_BUFFER_SIZE, " bytes");
    #endif
}

bool has_pending_data() {
    if (rtc_pool_length > 0) return true;
    
    if (ensure_sd_active()) {
        try {
            if (SD.exists(QUEUE_FILE)) {
                File file = SD.open(QUEUE_FILE, FILE_READ);
                if (file) {
                    size_t size = file.size();
                    file.close();
                    return size > 0;
                }
            }
        } catch(const std::exception& e) {
            local_logger("Excepción al verificar datos pendientes en SD.");
            add_sd_error(String("[HAS_PENDING_SD_ERROR: ") + e.what() + "] ");
        }
    } else {
        add_sd_error("[HAS_PENDING_FAIL: SD_NOT_ACTIVE] ");
    }
    return false;
}

String get_batch_payload() {
    current_batch_count = 0;
    batch_consumed_bytes = 0;
    String final_json = "{\"messages\": [";

    // PRIORIDAD 1: Leer de la memoria RTC
    if (rtc_pool_length > 0) {
        is_batch_from_rtc = true;
        int offset = 0;
        
        while (current_batch_count < MAX_BATCH_SIZE && offset < rtc_pool_length) {
            char* next_newline = strchr(rtc_payload_pool + offset, PAYLOAD_DELIMITER);
            if (next_newline) {
                int len = next_newline - (rtc_payload_pool + offset);
                // Extraer el JSON y agregarlo al lote
                String item = String(rtc_payload_pool + offset, len);

                int lastBrace = item.lastIndexOf('}');
                if (lastBrace != -1) {
                    item = item.substring(0, lastBrace) + ",\"ls\":\"RTC\"}";
                }
                
                current_batch_payloads[current_batch_count++] = item;
                
                if (current_batch_count > 1) final_json += ",";
                final_json += item;
                
                offset += len + 1; // +1 por el delimitador \n
            } else {
                break; // No hay más delimitadores
            }
        }
        batch_consumed_bytes = offset;
    } 
    // PRIORIDAD 2: Leer de la Tarjeta SD (Si RTC está vacía)
    else if (ensure_sd_active()) {
        try {
            if (SD.exists(QUEUE_FILE)) {
                is_batch_from_rtc = false;
                File file = SD.open(QUEUE_FILE, FILE_READ);
                if (file) {
                    while (file.available() && current_batch_count < MAX_BATCH_SIZE) {
                        String line = file.readStringUntil(PAYLOAD_DELIMITER);
                        line.trim();
                        if (line.length() > 0) {

                            int lastBrace = line.lastIndexOf('}');
                            if (lastBrace != -1) {
                                line = line.substring(0, lastBrace) + ",\"ls\":\"SD\"}";
                            }                            

                            current_batch_payloads[current_batch_count++] = line;
                            if (current_batch_count > 1) final_json += ",";
                            final_json += line;
                        }
                        batch_consumed_bytes = file.position();
                    }
                    file.close();
                }
            }
        } catch(const std::exception& e) {
            local_logger("Excepción al leer lote de SD.");
            add_sd_error(String("[GET_BATCH_SD_ERROR: ") + e.what() + "] ");
        }
    } else {
        add_sd_error("[GET_BATCH_FAIL: SD_NOT_ACTIVE] ");
    }

    final_json += "]}";
    
    // Si no hay nada, devolver string vacío
    if (current_batch_count == 0) return "";
    return final_json;
}

void commit_batch() {
    if (current_batch_count == 0) return;

    if (is_batch_from_rtc) {
        // Borrar los datos enviados de la memoria RTC (Desplazar a la izquierda)
        int remaining_bytes = rtc_pool_length - batch_consumed_bytes;
        if (remaining_bytes > 0) {
            memmove(rtc_payload_pool, rtc_payload_pool + batch_consumed_bytes, remaining_bytes);
        }
        rtc_pool_length = remaining_bytes;
        rtc_payload_pool[rtc_pool_length] = '\0'; // Asegurar fin de string
        local_logger("COMMIT: ", current_batch_count, " lecturas borradas de la RTC.");
    } 
    else if (ensure_sd_active()) {
        try {
            // Borrar los datos enviados de la SD usando un archivo staging
            File original = SD.open(QUEUE_FILE, FILE_READ);
            File staging = SD.open(STAGING_FILE, FILE_WRITE);
            
            if (original && staging) {
                original.seek(batch_consumed_bytes); // Saltar lo que ya enviamos
                uint8_t buffer[512];
                while (original.available()) {
                    int bytes_read = original.read(buffer, sizeof(buffer));
                    staging.write(buffer, bytes_read);
                }
                original.close();
                staging.close();
                
                SD.remove(QUEUE_FILE);
                SD.rename(STAGING_FILE, QUEUE_FILE);
                local_logger("COMMIT: Archivo SD actualizado. Lote eliminado.");
            }
        }
        catch(const std::exception& e){
            add_sd_error(String("[COMMIT_BATCH_FAIL: ") + e.what() + "] ");
        }
    }  else {
        add_sd_error("[COMMIT_BATCH_FAIL: SD_NOT_ACTIVE] ");
    }
    // Limpiar estado del lote
    current_batch_count = 0;
    batch_consumed_bytes = 0;
}

void rollback_batch() {
    if (current_batch_count == 0) return;

    local_logger("ROLLBACK: Falló el envío. Protegiendo datos...");
    
    // Si los datos venían de la RTC y la SD está sana, forzamos un derrame 
    // total a la SD para asegurar los datos antes del Deep Sleep.
    if (is_batch_from_rtc ) {
        if (ensure_sd_active()) {
            spillover_rtc_to_sd();
        } else {
            add_sd_error("[ROLLBACK_BATCH_FAIL: SD_NOT_ACTIVE] ");
        }
    }
    // Si la SD está dañada, simplemente no hacemos commit. 
    // Los datos seguirán existiendo en rtc_payload_pool para el próximo despertar.
    // Limpiar estado del lote
    current_batch_count = 0;
    batch_consumed_bytes = 0;
}