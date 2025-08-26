#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>
#include <notification/notification_messages.h>
#include <bt/bt_service_api.h>

#define TAG "BLE_Scanner"
#define MAX_DEVICES 32

// Structure pour un appareil simulé (en attendant vraies APIs)
typedef struct {
    char name[32];
    char addr[18];
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

// Simulation de scan BLE (APIs réelles non disponibles dans SDK public)
void ble_scanner_simulate_devices(BleScanner* app) {
    // Effacer les anciens résultats
    app->device_count = 0;
    
    // Ajouter des appareils simulés pour démonstration
    // En réalité, il faudrait accéder aux APIs BLE internes
    
    BleDevice* device1 = &app->devices[app->device_count++];
    snprintf(device1->name, sizeof(device1->name), "iPhone de John");
    snprintf(device1->addr, sizeof(device1->addr), "AA:BB:CC:DD:EE:FF");
    device1->rssi = -45;
    device1->active = true;
    
    BleDevice* device2 = &app->devices[app->device_count++];
    snprintf(device2->name, sizeof(device2->name), "Galaxy S24");
    snprintf(device2->addr, sizeof(device2->addr), "11:22:33:44:55:66");
    device2->rssi = -67;
    device2->active = true;
    
    BleDevice* device3 = &app->devices[app->device_count++];
    snprintf(device3->name, sizeof(device3->name), "AirPods Pro");
    snprintf(device3->addr, sizeof(device3->addr), "77:88:99:AA:BB:CC");
    device3->rssi = -32;
    device3->active = true;
    
    BleDevice* device4 = &app->devices[app->device_count++];
    snprintf(device4->name, sizeof(device4->name), "Tesla Model 3");
    snprintf(device4->addr, sizeof(device4->addr), "DD:EE:FF:00:11:22");
    device4->rssi = -78;
    device4->active = true;
    
    FURI_LOG_I(TAG, "Simulated %d BLE devices", app->device_count);
}

// Démarrer le scan (simulation)
void ble_scanner_start_scan(BleScanner* app) {
    if(app->scanning) return;
    
    FURI_LOG_I(TAG, "Starting BLE scan simulation...");
    
    app->scanning = true;
    notification_message(app->notifications, &sequence_blink_start_blue);
    
    // Simuler le scan avec des appareils fictifs
    ble_scanner_simulate_devices(app);
    
    // Simuler durée de scan
    furi_delay_ms(3000); // 3 secondes
    
    app->scanning = false;
    notification_message(app->notifications, &sequence_blink_stop);
    
    FURI_LOG_I(TAG, "BLE scan completed - found %d devices", app->device_count);
}

// Arrêter le scan
void ble_scanner_stop_scan(BleScanner* app) {
    if(!app->scanning) return;
    
    FURI_LOG_I(TAG, "Stopping BLE scan...");
    app->scanning = false;
    notification_message(app->notifications, &sequence_blink_stop);
}

// Formater les résultats pour affichage
void ble_scanner_format_results(BleScanner* app) {
    furi_string_reset(app->text_box_store);
    
    if(app->device_count == 0) {
        furi_string_cat_printf(app->text_box_store, 
                              "No devices found.\n"
                              "Note: This is a demo using\n"
                              "simulated devices.\n\n"
                              "Real BLE APIs are not\n"
                              "available in public SDK.\n\n"
                              "Press BACK to return.");
        return;
    }
    
    furi_string_cat_printf(app->text_box_store, "BLE Devices Found: %d\n\n", app->device_count);
    
    for(uint8_t i = 0; i < app->device_count; i++) {
        BleDevice* device = &app->devices[i];
        furi_string_cat_printf(app->text_box_store, 
                              "%d. %s\n"
                              "   MAC: %s\n"
                              "   RSSI: %d dBm\n\n",
                              i + 1,
                              device->name,
                              device->addr,
                              device->rssi);
    }
    
    furi_string_cat_printf(app->text_box_store, 
                          "Note: Demo with simulated data\n"
                          "Real BLE scanning requires\n"
                          "internal firmware APIs.\n\n"
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
    return view_dispatcher_send_custom_event(app->view_dispatcher, 42);
}

// Custom event callback
bool ble_scanner_custom_event_callback(void* context, uint32_t event) {
    BleScanner* app = context;
    bool consumed = false;
    
    switch(event) {
        case BleSubmenuIndexStartScan:
            ble_scanner_start_scan(app);
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
