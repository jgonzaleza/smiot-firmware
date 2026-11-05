#include <SPI.h>
#include <SD.h>
#include "queue_manager.h"
#include "app_utils.h" // Para is_sd_ready y las rutas de los archivos
#include "logger.h"    // Para local_logger

// ==============================================================================
// INSTANCIACIÓN DE VARIABLES GLOBALES (Declaradas como extern en el .h)
// The payload pool lives in g_rtc (RTC_NOINIT_ATTR), not RTC_DATA_ATTR.
// ==============================================================================

String current_batch_payloads[MAX_BATCH_SIZE];
int current_batch_count = 0;
bool is_batch_from_rtc = true;
int batch_consumed_bytes = 0; // Rastrea cuántos bytes leer y borrar del origen

extern bool is_sdcard_ok;

static const char* QUEUE_NEW_FILE = "/queue.new";
static const char* QUEUE_BAK_FILE = "/queue.bak";
static const char* OFFSET_FILE = "/queue.off";
static const char* OFFSET_TMP_FILE = "/queue.off.tmp";
static const uint32_t OFFSET_MAGIC = 0x514F4646UL;  // 'QOFF'
static uint32_t sd_read_offset = 0;

struct OffsetRec {
    uint32_t magic;
    uint32_t offset;
    uint32_t crc;
};

static uint32_t offset_crc(uint32_t magic, uint32_t offset) {
    return magic ^ offset ^ 0xA5A5A5A5UL;
}

static bool read_offset_file(const char* path, uint32_t* out) {
    if (!SD.exists(path)) return false;
    File file = SD.open(path, FILE_READ);
    if (!file) return false;
    OffsetRec rec;
    size_t n = file.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec));
    file.close();
    if (n != sizeof(rec) || rec.magic != OFFSET_MAGIC) return false;
    if (rec.crc != offset_crc(rec.magic, rec.offset)) return false;
    *out = rec.offset;
    return true;
}

static bool write_offset_file(uint32_t offset) {
    OffsetRec rec;
    rec.magic = OFFSET_MAGIC;
    rec.offset = offset;
    rec.crc = offset_crc(rec.magic, rec.offset);
    if (SD.exists(OFFSET_TMP_FILE)) SD.remove(OFFSET_TMP_FILE);
    File file = SD.open(OFFSET_TMP_FILE, FILE_WRITE);
    if (!file) return false;
    size_t n = file.write(reinterpret_cast<const uint8_t*>(&rec), sizeof(rec));
    file.flush();
    file.close();
    if (n != sizeof(rec)) {
        SD.remove(OFFSET_TMP_FILE);
        return false;
    }
    if (SD.exists(OFFSET_FILE)) SD.remove(OFFSET_FILE);
    if (!SD.rename(OFFSET_TMP_FILE, OFFSET_FILE)) {
        sd_read_offset = offset;
        return true;
    }
    sd_read_offset = offset;
    return true;
}

static void load_sd_offset() {
    uint32_t value = 0;
    if (read_offset_file(OFFSET_FILE, &value) || read_offset_file(OFFSET_TMP_FILE, &value)) {
        sd_read_offset = value;
    } else {
        sd_read_offset = 0;
    }
}

static uint32_t sd_queue_size() {
    if (!SD.exists(QUEUE_FILE)) return 0;
    File file = SD.open(QUEUE_FILE, FILE_READ);
    if (!file) return 0;
    uint32_t size = file.size();
    file.close();
    return size;
}

// Finish or discard a compact that was interrupted by reset.
static void recover_sd_queue_files() {
    bool has_q = SD.exists(QUEUE_FILE);
    bool has_new = SD.exists(QUEUE_NEW_FILE);
    bool has_bak = SD.exists(QUEUE_BAK_FILE);

    if (has_new && has_q && !has_bak) {
        SD.remove(QUEUE_NEW_FILE);
        has_new = false;
    }
    if (!has_q && has_new) {
        if (SD.rename(QUEUE_NEW_FILE, QUEUE_FILE)) {
            if (has_bak) SD.remove(QUEUE_BAK_FILE);
            write_offset_file(0);
        }
        return;
    }
    if (has_q && has_bak && !has_new) {
        SD.remove(QUEUE_BAK_FILE);
        write_offset_file(0);
    }
}

static bool compact_sd_queue() {
    if (!ensure_sd_active() || !SD.exists(QUEUE_FILE)) {
        write_offset_file(0);
        return true;
    }
    File in = SD.open(QUEUE_FILE, FILE_READ);
    if (!in) return false;
    uint32_t size = in.size();
    if (sd_read_offset > size) sd_read_offset = size;
    if (sd_read_offset > 0) in.seek(sd_read_offset);

    if (SD.exists(QUEUE_NEW_FILE)) SD.remove(QUEUE_NEW_FILE);
    File out = SD.open(QUEUE_NEW_FILE, FILE_WRITE);
    if (!out) {
        in.close();
        return false;
    }
    uint8_t buffer[512];
    bool copy_ok = true;
    while (in.available()) {
        int n = in.read(buffer, sizeof(buffer));
        if (n < 0) {
            copy_ok = false;
            break;
        }
        if (n == 0) break;
        if ((int)out.write(buffer, (size_t)n) != n) {
            copy_ok = false;
            break;
        }
        feed_watchdog();
    }
    out.flush();
    out.close();
    in.close();
    if (!copy_ok) {
        SD.remove(QUEUE_NEW_FILE);
        return false;
    }
    if (SD.exists(QUEUE_BAK_FILE)) SD.remove(QUEUE_BAK_FILE);
    if (!SD.rename(QUEUE_FILE, QUEUE_BAK_FILE)) {
        SD.remove(QUEUE_NEW_FILE);
        return false;
    }
    if (!SD.rename(QUEUE_NEW_FILE, QUEUE_FILE)) {
        SD.rename(QUEUE_BAK_FILE, QUEUE_FILE);
        return false;
    }
    write_offset_file(0);
    SD.remove(QUEUE_BAK_FILE);
    return true;
}

static void enforce_sd_cap() {
    if (!SD.exists(QUEUE_FILE)) return;
    File file = SD.open(QUEUE_FILE, FILE_READ);
    if (!file) return;
    uint32_t size = file.size();
    if (sd_read_offset > size) sd_read_offset = size;
    uint32_t live = size - sd_read_offset;
    if (live > SD_QUEUE_CAP_BYTES) {
        if (!file.seek(sd_read_offset)) {
            file.close();
            return;
        }
        while (file.available() && (file.size() - file.position()) > SD_QUEUE_CAP_BYTES) {
            file.readStringUntil(PAYLOAD_DELIMITER);
            feed_watchdog();
        }
        sd_read_offset = file.position();
    }
    file.close();
    write_offset_file(sd_read_offset);
    if (sd_read_offset >= SD_QUEUE_COMPACT_WASTE) {
        compact_sd_queue();
    }
}

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
            enforce_sd_cap();
            // Vaciamos la RTC
            rtc_pool_length = 0;
            rtc_payload_pool[0] = '\0';
            rtc_persist_commit();
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
        if (remaining_bytes < 0 || bytes_to_remove <= 0) {
            rtc_pool_length = 0;
            rtc_payload_pool[0] = '\0';
            rtc_persist_commit();
            return;
        }
        
        // Desplazamos la memoria hacia la izquierda (sobrescribiendo el viejo)
        if (remaining_bytes > 0) {
            memmove(rtc_payload_pool, first_newline + 1, (size_t)remaining_bytes);
        }
        rtc_pool_length = remaining_bytes;
        rtc_payload_pool[rtc_pool_length] = '\0';
        rtc_persist_commit();
        local_logger("RTC LLENA (SIN SD): Dato más viejo descartado (Ring Buffer).");
    } else {
        // Fallback por si hay un string malformado sin delimitador
        rtc_pool_length = 0; 
        rtc_payload_pool[0] = '\0';
        rtc_persist_commit();
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
            recover_sd_queue_files();
            load_sd_offset();
            uint32_t queued = sd_queue_size();
            if (sd_read_offset > queued) {
                sd_read_offset = queued;
                write_offset_file(sd_read_offset);
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
    if (payload.length() == 0) return;
    payload += PAYLOAD_DELIMITER;
    int payload_len = payload.length();

    // A single record must fit in the pool, including the trailing NUL.
    if (payload_len <= 0 || payload_len >= MAX_RTC_BUFFER_SIZE) {
        add_sd_error("[QP_REJECT: OVERSIZE]");
        return;
    }

    // 2. Verificar si cabe en la memoria RTC. Re-check after spillover;
    //    drop the oldest record only as the fallback. Never strcpy past the pool.
    int guard = 0;
    while ((rtc_pool_length + payload_len) >= MAX_RTC_BUFFER_SIZE) {
        if (++guard > 64) {
            add_sd_error("[QP_REJECT: NO_ROOM]");
            return;
        }
        int before = rtc_pool_length;
        bool spilled = false;
        if (ensure_sd_active()) {
            spillover_rtc_to_sd();
            spilled = rtc_pool_length < before;
        } else {
            add_sd_error("[QP_FAIL: SD_NOT_ACTIVE]");
        }
        if ((rtc_pool_length + payload_len) < MAX_RTC_BUFFER_SIZE) break;
        drop_oldest_rtc_record();
        if (rtc_pool_length >= before && !spilled) {
            add_sd_error("[QP_REJECT: NO_ROOM]");
            return;
        }
    }

    if ((rtc_pool_length + payload_len) >= MAX_RTC_BUFFER_SIZE) {
        add_sd_error("[QP_REJECT: NO_ROOM]");
        return;
    }

    // 3. Insertar el nuevo payload al final del pool de la RTC
    memcpy(rtc_payload_pool + rtc_pool_length, payload.c_str(), (size_t)payload_len);
    rtc_pool_length += payload_len;
    rtc_payload_pool[rtc_pool_length] = '\0';
    rtc_persist_commit();
    
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
                    return size > sd_read_offset;
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
                    if (sd_read_offset > 0 && sd_read_offset < file.size()) {
                        file.seek(sd_read_offset);
                    } else if (sd_read_offset >= file.size()) {
                        file.close();
                        final_json += "]}";
                        if (current_batch_count == 0) return "";
                        return final_json;
                    }
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
        if (batch_consumed_bytes < 0 || batch_consumed_bytes > rtc_pool_length) {
            batch_consumed_bytes = rtc_pool_length;
        }
        int remaining_bytes = rtc_pool_length - batch_consumed_bytes;
        if (remaining_bytes > 0 && batch_consumed_bytes > 0) {
            memmove(rtc_payload_pool, rtc_payload_pool + batch_consumed_bytes, (size_t)remaining_bytes);
        }
        if (remaining_bytes < 0) remaining_bytes = 0;
        rtc_pool_length = remaining_bytes;
        rtc_payload_pool[rtc_pool_length] = '\0'; // Asegurar fin de string
        rtc_persist_commit();
        local_logger("COMMIT: ", current_batch_count, " lecturas borradas de la RTC.");
    } 
    else if (ensure_sd_active()) {
        try {
            // Advance a read offset. The file is not rewritten for every batch.
            // Compact only when the discarded prefix is large or the cap is hit.
            if (batch_consumed_bytes < 0) batch_consumed_bytes = 0;
            sd_read_offset = (uint32_t)batch_consumed_bytes;
            write_offset_file(sd_read_offset);
            uint32_t size = sd_queue_size();
            if (sd_read_offset >= SD_QUEUE_COMPACT_WASTE || size > SD_QUEUE_CAP_BYTES) {
                enforce_sd_cap();
            }
            local_logger("COMMIT: Offset SD actualizado. Lote eliminado.");
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