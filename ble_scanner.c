#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/widget.h>
#include <notification/notification_messages.h>
#include <bt/bt_service/bt_i.h>
#include <ble/ble.h>

#define TAG "BLE_Scanner"
#define MAX_DEVICES 32
#define SCAN_DURATION 10000 // 10 secondes

// Structure pour un appareil découvert
typedef struct {
    uint8_t addr[6];
    char name[32];
    int8_t rssi;
    bool active;
} BleDevice;

// Structure principale de l'app
typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    Widget* widget;
    NotificationApp* notifications;
    
    BleDevice devices[MAX_DEVICES];
    uint8_t device_count;
    bool scanning;
    uint32_t scan_start_time;
    
    FuriString* text_box_store;
} BleScanner;

// IDs des vues
typedef enum {
    BleSceneScannerSubmenu,
    BleSceneScannerWidget,
} BleSceneIndex;

typedef enum {
    BleSceneScannerView,
    BleSceneScannerSubmenuView,
} BleView;

enum BleSubmenuIndex {
    BleSubmenuIndexStartScan,
    BleSubmenuIndexShowResults,
    BleSubmenuIndexClearResults,
};

// Callback pour les appareils découverts
void ble_scan_result_callback(
    const uint8_t* addr,
    int8_t rssi,
    const uint8_t* adv_data,
    uint8_t adv_data_len,
    void* context) {
    
    BleScanner* app = (BleScanner*)context;
    
    if(app->device_count >= MAX_DEVICES) return;
    
    // Vérifier si l'appareil existe déjà
    for(uint8_t i = 0; i < app->device_count; i++) {
        if(memcmp(app->devices[i].addr, addr, 6) == 0) {
            // Mettre à jour le RSSI
            app->devices[i].rssi = rssi;
            return;
        }
    }
    
    // Nouvel appareil
    BleDevice* device = &app->devices[app->device_count];
    memcpy(device->addr, addr, 6);
    device->rssi = rssi;
    device->active = true;
    
    // Extraire le nom depuis les données publicitaires
    snprintf(device->name, sizeof(device->name), "Unknown");
    
    // Parser les données publicitaires pour trouver le nom
    uint8_t pos = 0;
    while(pos < adv_data_len) {
        uint8_t len = adv_data[pos];
        if(len == 0) break;
        
        uint8_t type = adv_data[pos + 1];
        if(type == 0x08 || type == 0x09) { // Complete/Shortened Local Name
            uint8_t name_len = len - 1;
            if(name_len > 0 && name_len < sizeof(device->name)) {
                memcpy(device->name, &adv_data[pos + 2], name_len);
                device->name[name_len] = '\0';
            }
            break;
        }
        pos += len + 1;
    }
    
    app->device_count++;
    FURI_LOG_I(TAG, "Found device: %s (%02X:%02X:%02X:%02X:%02X:%02X) RSSI: %d", 
               device->name,
               device->addr[0], device->addr[1], device->addr[2],
               device->addr[3], device->addr[4], device->addr[5],
               device->rssi);
}

// Démarrer le scan
void ble_scanner_start_scan(BleScanner* app) {
    if(app->scanning) return;
    
    FURI_LOG_I(TAG, "Starting BLE scan...");
    
    // Effacer les anciens résultats
    app->device_count = 0;
    memset(app->devices, 0, sizeof(app->devices));
    
    // Démarrer le scan BLE
    app->scanning = true;
    app->scan_start_time = furi_get_tick();
    
    // Configuration du scan
    BtStatus status = bt_set_status_changed_callback(NULL, NULL);
    if(status == BtStatusReady) {
        // Démarrer le scan avec callback
        ble_gap_start_scan(ble_scan_result_callback, app);
    }
    
    notification_message(app->notifications, &sequence_blink_start_blue);
}

// Arrêter le scan
void ble_scanner_stop_scan(BleScanner* app) {
    if(!app->scanning) return;
    
    FURI_LOG_I(TAG, "Stopping BLE scan...");
    
    ble_gap_stop_scan();
    app->scanning = false;
    
    notification_message(app->notifications, &sequence_blink_stop);
}

// Formater les résultats pour affichage
void ble_scanner_format_results(BleScanner* app) {
    furi_string_reset(app->text_box_store);
    
    if(app->device_count == 0) {
        furi_string_cat_printf(app->text_box_store, "No devices found.\nPress BACK to return.");
        return;
    }
    
    furi_string_cat_printf(app->text_box_store, "Found %d devices:\n\n", app->device_count);
    
    for(uint8_t i = 0; i < app->device_count; i++) {
        BleDevice* device = &app->devices[i];
        furi_string_cat_printf(app->text_box_store, 
                              "%d. %s\n"
                              "   %02X:%02X:%02X:%02X:%02X:%02X\n"
                              "   RSSI: %d dBm\n\n",
                              i + 1,
                              device->name,
                              device->addr[0], device->addr[1], device->addr[2],
                              device->addr[3], device->addr[4], device->addr[5],
                              device->rssi);
    }
    
    furi_string_cat_printf(app->text_box_store, "Press BACK to return.");
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
    return view_dispatcher_send_custom_event(app->view_dispatcher, 42);
}

// Custom event callback
bool ble_scanner_custom_event_callback(void* context, uint32_t event) {
    BleScanner* app = context;
    bool consumed = false;
    
    switch(event) {
        case BleSubmenuIndexStartScan:
            ble_scanner_start_scan(app);
            // Timer pour arrêter le scan après 10 secondes
            furi_delay_ms(SCAN_DURATION);
            ble_scanner_stop_scan(app);
            consumed = true;
            break;
            
        case BleSubmenuIndexShowResults:
            ble_scanner_format_results(app);
            widget_reset(app->widget);
            widget_add_text_scroll_element(app->widget, 0, 0, 128, 64, 
                                         furi_string_get_cstr(app->text_box_store));
            view_dispatcher_switch_to_view(app->view_dispatcher, BleSceneScannerWidget);
            consumed = true;
            break;
            
        case BleSubmenuIndexClearResults:
            app->device_count = 0;
            memset(app->devices, 0, sizeof(app->devices));
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
    app->text_box_store = furi_string_alloc();
    
    view_dispatcher_enable_queue(app->view_dispatcher);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, ble_scanner_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, ble_scanner_navigation_event_callback);
    
    // Configuration du submenu
    submenu_add_item(app->submenu, "Start Scan", BleSubmenuIndexStartScan, ble_scanner_submenu_callback, app);
    submenu_add_item(app->submenu, "Show Results", BleSubmenuIndexShowResults, ble_scanner_submenu_callback, app);
    submenu_add_item(app->submenu, "Clear Results", BleSubmenuIndexClearResults, ble_scanner_submenu_callback, app);
    
    view_dispatcher_add_view(app->view_dispatcher, BleSceneScannerSubmenuView, submenu_get_view(app->submenu));
    view_dispatcher_add_view(app->view_dispatcher, BleSceneScannerWidget, widget_get_view(app->widget));
    
    // Initialiser les données
    app->device_count = 0;
    app->scanning = false;
    
    return app;
}

// Libération de l'app
static void ble_scanner_free(BleScanner* app) {
    ble_scanner_stop_scan(app);
    
    view_dispatcher_remove_view(app->view_dispatcher, BleSceneScannerWidget);
    view_dispatcher_remove_view(app->view_dispatcher, BleSceneScannerSubmenuView);
    
    widget_free(app->widget);
    submenu_free(app->submenu);
    view_dispatcher_free(app->view_dispatcher);
    
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
