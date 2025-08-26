#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>
#include <gui/modules/text_box.h>
#include <notification/notification_messages.h>
#include <furi_hal_serial.h>

#define TAG "BLE_Scanner"
#define MAX_DEVICES 50
#define BAUDRATE 115200
#define RX_BUF_SIZE 2048

// Structure pour un appareil BLE réel
typedef struct {
    char name[64];
    char mac[18];
    int8_t rssi;
    char vendor[32];
    bool active;
} BleDevice;

// Structure principale de l'app
typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    Widget* widget;
    TextBox* text_box;
    NotificationApp* notifications;
    
    FuriHalSerialHandle* serial_handle;
    
    BleDevice devices[MAX_DEVICES];
    uint16_t device_count;
    bool scanning;
    bool marauder_connected;
    
    FuriString* text_box_store;
    FuriStreamBuffer* rx_stream;
    FuriThread* worker_thread;
    volatile bool worker_running;
    
} BleScanner;

// IDs des vues
typedef enum {
    BleSceneScannerSubmenu,
    BleSceneScannerWidget,
    BleSceneScannerTextBox,
} BleSceneIndex;

typedef enum {
    BleSceneScannerView,
    BleSceneScannerSubmenuView,
    BleSceneScannerTextBoxView,
} BleView;

enum BleSubmenuIndex {
    BleSubmenuIndexScanDevices,
    BleSubmenuIndexShowResults,
    BleSubmenuIndexClearResults,
    BleSubmenuIndexMarauderStatus,
};

// Callback UART pour recevoir des données du Marauder
void uart_on_irq_cb(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* context) {
    UNUSED(handle);
    BleScanner* app = (BleScanner*)context;
    
    if(event == FuriHalSerialRxEventData) {
        uint8_t data = furi_hal_serial_async_rx(app->serial_handle);
        furi_stream_buffer_send(app->rx_stream, &data, 1, 0);
        
        // Signal worker thread that data is available
        if(app->worker_thread) {
            furi_thread_flags_set(furi_thread_get_id(app->worker_thread), (1 << 0));
        }
    }
}

// Parser une ligne de résultat BLE du Marauder
bool parse_ble_device(const char* line, BleDevice* device) {
    // Format Marauder: "BLE: MAC RSSI Name [Vendor]"
    // Exemple: "BLE: AA:BB:CC:DD:EE:FF -45 iPhone de John [Apple]"
    
    if(strncmp(line, "BLE:", 4) != 0) return false;
    
    char mac[18], name[64], vendor[32];
    int rssi;
    
    // Parser la ligne avec sscanf
    int parsed = sscanf(line + 5, "%17s %d %63s [%31[^]]]", mac, &rssi, name, vendor);
    
    if(parsed >= 3) {
        strncpy(device->mac, mac, sizeof(device->mac) - 1);
        device->rssi = (int8_t)rssi;
        strncpy(device->name, name, sizeof(device->name) - 1);
        
        if(parsed >= 4) {
            strncpy(device->vendor, vendor, sizeof(device->vendor) - 1);
        } else {
            strncpy(device->vendor, "Unknown", sizeof(device->vendor) - 1);
        }
        
        device->active = true;
        return true;
    }
    
    return false;
}

// Worker thread pour traiter les données UART
int32_t uart_worker(void* context) {
    BleScanner* app = (BleScanner*)context;
    uint8_t data[256];
    char line_buffer[512];
    uint16_t line_pos = 0;
    
    FURI_LOG_I(TAG, "UART Worker started");
    
    while(app->worker_running) {
        uint32_t events = furi_thread_flags_wait(
            (1 << 0) | (1 << 1), // DataWaiting | Exiting  
            FuriFlagWaitAny, 
            100); // 100ms timeout
            
        if(events & (1 << 1)) { // Exiting
            break;
        }
        
        if(events & (1 << 0)) { // DataWaiting
            size_t bytes_read = furi_stream_buffer_receive(app->rx_stream, data, sizeof(data), 0);
            
            for(size_t i = 0; i < bytes_read; i++) {
                char c = (char)data[i];
                
                if(c == '\n' || c == '\r') {
                    if(line_pos > 0) {
                        line_buffer[line_pos] = '\0';
                        
                        // Parser la ligne pour détecter des appareils BLE
                        BleDevice temp_device = {0};
                        if(parse_ble_device(line_buffer, &temp_device)) {
                            
                            // Vérifier si l'appareil existe déjà
                            bool found = false;
                            for(uint16_t j = 0; j < app->device_count; j++) {
                                if(strcmp(app->devices[j].mac, temp_device.mac) == 0) {
                                    // Mettre à jour RSSI
                                    app->devices[j].rssi = temp_device.rssi;
                                    found = true;
                                    break;
                                }
                            }
                            
                            // Ajouter nouveau device si pas trouvé et place disponible
                            if(!found && app->device_count < MAX_DEVICES) {
                                app->devices[app->device_count] = temp_device;
                                app->device_count++;
                                FURI_LOG_I(TAG, "BLE Device found: %s (%s) %d dBm", 
                                          temp_device.name, temp_device.mac, temp_device.rssi);
                            }
                        }
                        
                        line_pos = 0;
                    }
                } else if(line_pos < sizeof(line_buffer) - 1) {
                    line_buffer[line_pos++] = c;
                }
            }
        }
    }
    
    FURI_LOG_I(TAG, "UART Worker stopped");
    return 0;
}

// Envoyer une commande au Marauder
void send_marauder_command(BleScanner* app, const char* command) {
    if(app->serial_handle) {
        furi_hal_serial_tx(app->serial_handle, (uint8_t*)command, strlen(command));
        furi_hal_serial_tx(app->serial_handle, (uint8_t*)"\r\n", 2);
        furi_delay_ms(100);
    }
}

// Vérifier la connexion Marauder
bool check_marauder_connection(BleScanner* app) {
    // Vider le buffer
    furi_stream_buffer_reset(app->rx_stream);
    
    // Envoyer commande de statut
    send_marauder_command(app, "help");
    
    // Attendre réponse
    furi_delay_ms(1000);
    
    uint8_t data[256];
    size_t bytes = furi_stream_buffer_receive(app->rx_stream, data, sizeof(data), 0);
    
    if(bytes > 0) {
        data[bytes] = '\0';
        if(strstr((char*)data, "Marauder") || strstr((char*)data, "help")) {
            app->marauder_connected = true;
            return true;
        }
    }
    
    app->marauder_connected = false;
    return false;
}

// Démarrer le scan BLE réel
void ble_scanner_start_real_scan(BleScanner* app) {
    if(app->scanning) return;
    
    FURI_LOG_I(TAG, "Starting REAL BLE scan via Marauder...");
    
    if(!check_marauder_connection(app)) {
        FURI_LOG_E(TAG, "Marauder not connected!");
        return;
    }
    
    // Effacer anciens résultats
    app->device_count = 0;
    memset(app->devices, 0, sizeof(app->devices));
    
    app->scanning = true;
    notification_message(app->notifications, &sequence_blink_start_cyan);
    
    // Commande pour scanner BLE avec Marauder
    send_marauder_command(app, "scanap -t bt");
    
    // Scanner pendant 15 secondes
    furi_delay_ms(15000);
    
    // Arrêter le scan
    send_marauder_command(app, "stopscan");
    
    app->scanning = false;
    notification_message(app->notifications, &sequence_blink_stop);
    
    FURI_LOG_I(TAG, "BLE scan completed - found %d REAL devices", app->device_count);
}

// Arrêter le scan
void ble_scanner_stop_scan(BleScanner* app) {
    if(!app->scanning) return;
    
    FURI_LOG_I(TAG, "Stopping BLE scan...");
    
    send_marauder_command(app, "stopscan");
    app->scanning = false;
    
    notification_message(app->notifications, &sequence_blink_stop);
}

// Formater les résultats réels
void ble_scanner_format_real_results(BleScanner* app) {
    furi_string_reset(app->text_box_store);
    
    const char* marauder_status = app->marauder_connected ? "Connected" : "Disconnected";
    
    if(app->device_count == 0) {
        furi_string_cat_printf(app->text_box_store, 
                              "No BLE devices found.\n"
                              "Marauder: %s\n\n"
                              "Make sure:\n"
                              "- ESP32 Marauder is connected\n"
                              "- GPIO pins are wired correctly\n"
                              "- BLE devices are nearby\n"
                              "- Devices are discoverable\n\n"
                              "Press BACK to return.", marauder_status);
        return;
    }
    
    furi_string_cat_printf(app->text_box_store, "REAL BLE Devices: %d\n", app->device_count);
    furi_string_cat_printf(app->text_box_store, "Marauder: %s\n\n", marauder_status);
    
    for(uint16_t i = 0; i < app->device_count; i++) {
        BleDevice* device = &app->devices[i];
        furi_string_cat_printf(app->text_box_store, 
                              "%d. %s\n"
                              "   MAC: %s\n"
                              "   RSSI: %d dBm\n"
                              "   Vendor: %s\n\n",
                              i + 1,
                              device->name[0] ? device->name : "Unknown",
                              device->mac,
                              device->rssi,
                              device->vendor);
    }
    
    furi_string_cat_printf(app->text_box_store, 
                          "Scanned via ESP32 Marauder\n"
                          "Real Bluetooth devices detected!\n\n"
                          "Press BACK to return.");
}

// Callback du widget
bool ble_scanner_widget_callback(GuiButtonType result, InputType type, void* context) {
    BleScanner* app = context;
    if(type == InputTypeShort) {
        view_dispatcher_send_custom_event(app->view_dispatcher, result);
    }
    return true;
}

// Callback du submenu
void ble_scanner_submenu_callback(void* context, uint32_t index) {
    BleScanner* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

// Navigation callback
bool ble_scanner_navigation_event_callback(void* context) {
    BleScanner* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, 42);
    return true;
}

// Custom event callback
bool ble_scanner_custom_event_callback(void* context, uint32_t event) {
    BleScanner* app = context;
    bool consumed = false;
    
    switch(event) {
        case BleSubmenuIndexScanDevices:
            ble_scanner_start_real_scan(app);
            consumed = true;
            break;
            
        case BleSubmenuIndexShowResults:
            ble_scanner_format_real_results(app);
            text_box_reset(app->text_box);
            text_box_set_text(app->text_box, furi_string_get_cstr(app->text_box_store));
            view_dispatcher_switch_to_view(app->view_dispatcher, BleSceneScannerTextBoxView);
            consumed = true;
            break;
            
        case BleSubmenuIndexClearResults:
            app->device_count = 0;
            memset(app->devices, 0, sizeof(app->devices));
            consumed = true;
            break;
            
        case BleSubmenuIndexMarauderStatus:
            furi_string_reset(app->text_box_store);
            bool connected = check_marauder_connection(app);
            furi_string_cat_printf(app->text_box_store,
                                  "ESP32 Marauder Status:\n"
                                  "Connection: %s\n\n"
                                  "GPIO Wiring (USART):\n"
                                  "ESP32 TX -> Flipper Pin 13 (RX)\n"
                                  "ESP32 RX -> Flipper Pin 14 (TX)\n"
                                  "ESP32 GND -> Flipper Pin 11 (GND)\n"
                                  "ESP32 3.3V -> Flipper Pin 9 (3.3V)\n\n"
                                  "Make sure:\n"
                                  "- ESP32 Marauder firmware installed\n"
                                  "- GPIO connections secure\n"
                                  "- Baud rate: 115200\n\n"
                                  "Commands available:\n"
                                  "- scanap -t bt (BLE scan)\n"
                                  "- stopscan\n"
                                  "- help\n\n"
                                  "Press BACK to return.",
                                  connected ? "CONNECTED" : "DISCONNECTED");
            text_box_reset(app->text_box);
            text_box_set_text(app->text_box, furi_string_get_cstr(app->text_box_store));
            view_dispatcher_switch_to_view(app->view_dispatcher, BleSceneScannerTextBoxView);
            consumed = true;
            break;
            
        case 42: // Back button
            view_dispatcher_switch_to_view(app->view_dispatcher, BleSceneScannerSubmenuView);
            consumed = true;
            break;
    }
    
    return consumed;
}

// Allocation de l'app
static BleScanner* ble_scanner_alloc() {
    BleScanner* app = malloc(sizeof(BleScanner));
    
    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    
    app->view_dispatcher = view_dispatcher_alloc();
    app->submenu = submenu_alloc();
    app->widget = widget_alloc();
    app->text_box = text_box_alloc();
    app->text_box_store = furi_string_alloc();
    view_dispatcher_add_view(app->view_dispatcher, BleSceneScannerSubmenuView, submenu_get_view(app->submenu));
    view_dispatcher_add_view(app->view_dispatcher, BleSceneScannerWidget, widget_get_view(app->widget));
    view_dispatcher_add_view(app->view_dispatcher, BleSceneScannerTextBoxView, text_box_get_view(app->text_box));
    
    app->rx_stream = furi_stream_buffer_alloc(RX_BUF_SIZE, 1);
    
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, ble_scanner_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, ble_scanner_navigation_event_callback);
    
    // Configuration du submenu
    submenu_add_item(app->submenu, "Scan BLE Devices", BleSubmenuIndexScanDevices, ble_scanner_submenu_callback, app);
    submenu_add_item(app->submenu, "Show Results", BleSubmenuIndexShowResults, ble_scanner_submenu_callback, app);
    submenu_add_item(app->submenu, "Clear Results", BleSubmenuIndexClearResults, ble_scanner_submenu_callback, app);
    submenu_add_item(app->submenu, "Marauder Status", BleSubmenuIndexMarauderStatus, ble_scanner_submenu_callback, app);
    
    view_dispatcher_add_view(app->view_dispatcher, BleSceneScannerSubmenuView, submenu_get_view(app->submenu));
    view_dispatcher_add_view(app->view_dispatcher, BleSceneScannerWidget, widget_get_view(app->widget));
    view_dispatcher_add_view(app->view_dispatcher, BleSceneScannerTextBoxView, text_box_get_view(app->text_box));
    
    // Configuration UART pour ESP32 Marauder
    app->serial_handle = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    
    if(app->serial_handle) {
        furi_hal_serial_init(app->serial_handle, BAUDRATE);
        furi_hal_serial_async_rx_start(app->serial_handle, uart_on_irq_cb, app, false);
    }
    
    // Démarrer worker thread
    app->worker_running = true;
    app->worker_thread = furi_thread_alloc_ex("BLEScannerWorker", 2048, uart_worker, app);
    furi_thread_start(app->worker_thread);
    
    // Initialiser les données
    app->device_count = 0;
    app->scanning = false;
    app->marauder_connected = false;
    
    // Test connexion Marauder
    furi_delay_ms(1000);
    check_marauder_connection(app);
    
    return app;
}

// Libération de l'app
static void ble_scanner_free(BleScanner* app) {
    ble_scanner_stop_scan(app);
    
    // Arrêter worker
    if(app->worker_thread) {
        app->worker_running = false;
        furi_thread_flags_set(furi_thread_get_id(app->worker_thread), (1 << 1)); // Exiting flag
        furi_thread_join(app->worker_thread);
        furi_thread_free(app->worker_thread);
    }
    
    // Nettoyer UART
    if(app->serial_handle) {
        furi_hal_serial_async_rx_stop(app->serial_handle);
        furi_hal_serial_deinit(app->serial_handle);
        furi_hal_serial_control_release(app->serial_handle);
    }
    
    view_dispatcher_remove_view(app->view_dispatcher, BleSceneScannerTextBoxView);
    view_dispatcher_remove_view(app->view_dispatcher, BleSceneScannerWidget);
    view_dispatcher_remove_view(app->view_dispatcher, BleSceneScannerSubmenuView);
    
    text_box_free(app->text_box);
    widget_free(app->widget);
    submenu_free(app->submenu);
    view_dispatcher_free(app->view_dispatcher);
    
    furi_stream_buffer_free(app->rx_stream);
    furi_string_free(app->text_box_store);
    
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    
    free(app);
}

// Point d'entrée principal
int32_t ble_scanner_app(void* p) {
    UNUSED(p);
    
    BleScanner* app = ble_scanner_alloc();
    
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->view_dispatcher, BleSceneScannerSubmenuView);
    
    view_dispatcher_run(app->view_dispatcher);
    
    ble_scanner_free(app);
    
    return 0;
}
