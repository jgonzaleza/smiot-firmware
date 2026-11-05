#ifndef QUEUE_MANAGER_H
#define QUEUE_MANAGER_H

#include <Arduino.h>
#include "rtc_persist.h"

// ==============================================================================
// CONFIGURACIÓN DE LA COLA RESILIENTE
// ==============================================================================
#define MAX_RTC_BUFFER_SIZE RTC_POOL_BYTES
#define MAX_BATCH_SIZE 4        // Máximo de lecturas por mensaje MQTT
#define PAYLOAD_DELIMITER '\n'   // Separador de JSONs en nuestro pool
#define SD_QUEUE_CAP_BYTES (8UL * 1024UL * 1024UL)
#define SD_QUEUE_COMPACT_WASTE 65536UL

// Variables de estado del Batch actual en memoria RAM (Se borran al dormir)
extern String current_batch_payloads[MAX_BATCH_SIZE];
extern int current_batch_count;
extern bool is_batch_from_rtc;   // Indica si el lote actual salió de la RTC o SD

// ==============================================================================
// INTERFAZ DEL GESTOR (Queue Manager API)
// ==============================================================================

/**
 * Inicializa el gestor (se debe llamar en el setup).
 */
void queue_manager_init();

/**
 * Añade una nueva lectura a la cola. 
 * Maneja automáticamente la memoria RTC, el derrame a SD (Spillover) 
 * y el búfer circular si todo falla.
 */
void queue_payload(String payload);

/**
 * Verifica si hay datos guardados (en RTC o SD) esperando ser enviados.
 */
bool has_pending_data();

/**
 * Extrae hasta MAX_BATCH_SIZE lecturas de la RTC o la SD y las empaqueta 
 * en un String con formato de arreglo JSON: "[{...}, {...}]".
 */
String get_batch_payload();

/**
 * Confirma que el envío fue exitoso. Elimina definitivamente el lote 
 * que se acaba de extraer (lo borra de la RTC o de la SD).
 */
void commit_batch();

/**
 * Se llama si falló el envío MQTT. 
 * Si los datos venían de la RTC y la SD funciona, los refugia en la SD.
 * Si no, los deja en la RTC para el próximo intento.
 */
void rollback_batch();

// ==============================================================================
// FUNCIONES PRIVADAS / HELPER (Solo para uso interno del .cpp)
// ==============================================================================
void spillover_rtc_to_sd();
void drop_oldest_rtc_record();

#endif