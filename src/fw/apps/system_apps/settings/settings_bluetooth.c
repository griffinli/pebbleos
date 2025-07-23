/*
 * Copyright 2024 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define FILE_LOG_COLOR LOG_COLOR_BLUE

#include "settings_bluetooth.h"
#include "settings_menu.h"
#include "settings_remote.h"
#include "settings_window.h"

#include "applib/app.h"
#include "applib/app_focus_service.h"
#include "applib/event_service_client.h"
#include "applib/fonts/fonts.h"
#include "applib/graphics/graphics.h"
#include "applib/graphics/gtypes.h"
#include "applib/ui/ui.h"
#include "comm/bt_lock.h"
#include "comm/ble/gap_le_connection.h"
#include "comm/ble/gap_le_device_name.h"
#include "drivers/rtc.h"
#include "kernel/pbl_malloc.h"
#include "kernel/ui/system_icons.h"
#include "resource/resource_ids.auto.h"
#include "services/common/analytics/analytics.h"
#include "services/common/bluetooth/bluetooth_persistent_storage.h"
#include "services/common/bluetooth/local_id.h"
#include "services/common/bluetooth/pairability.h"
#include "services/common/i18n/i18n.h"
#include "services/common/system_task.h"
#include "services/normal/bluetooth/ble_hrm.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/string.h"

#include <bluetooth/bluetooth_types.h>
#include <bluetooth/classic_connect.h>
#include <bluetooth/reconnect.h>
#include <bluetooth/sm_types.h>
#include <btutil/bt_device.h>

#include <stdio.h>
#include <string.h>

#define HEADER_BUFFER_SIZE 22

#define SHARING_HEART_RATE_EXTRA_HEIGHT_PX (18)

typedef enum SettingsBluetooth {
  SettingsBluetoothAirplaneMode,
  SettingsBluetoothTotal,
} SettingsBluetooth;

enum {
  BluetoothIconIdx,
  BluetoothAltIconIdx,
  AirplaneIconIdx,
  NumIcons,
};

static const uint32_t ICON_RESOURCE_ID[NumIcons] = {
  RESOURCE_ID_SETTINGS_ICON_BLUETOOTH,
  RESOURCE_ID_SETTINGS_ICON_BLUETOOTH_ALT,
  RESOURCE_ID_SETTINGS_ICON_AIRPLANE,
};

typedef enum {
  ToggleStateIdle,
  ToggleStateEnablingBluetooth,
  ToggleStateDisablingBluetooth,
} ToggleState;

typedef struct SettingsBluetoothData {
  SettingsCallbacks callbacks;

  GBitmap icon_heap_bitmap[NumIcons];

  ListNode* remote_list_head;

  char header_buffer[HEADER_BUFFER_SIZE];
  ToggleState toggle_state;

  EventServiceInfo bt_airplane_event_info;
  EventServiceInfo bt_connection_event_info;
  EventServiceInfo bt_pairing_event_info;
  EventServiceInfo ble_device_name_updated_event_info;
#if CAPABILITY_HAS_BUILTIN_HRM
  EventServiceInfo ble_hrm_sharing_event_info;
#endif

  bool pairable;
} SettingsBluetoothData;

// BT stack interaction stuff
///////////////////////////

static void settings_bluetooth_reconnect_once(void) {
  // After the user toggles BT back on, immediately attempt to reconnect once:
  if (bt_ctl_is_airplane_mode_on() == false) {
    bt_driver_reconnect_try_now(true /*ignore_paused*/);
  }
}

static void settings_bluetooth_toggle_airplane_mode(SettingsBluetoothData* data) {
  const bool airplane_mode = bt_ctl_is_airplane_mode_on();
  bt_ctl_set_airplane_mode_async(!airplane_mode);
  data->toggle_state =
      airplane_mode ? ToggleStateEnablingBluetooth : ToggleStateDisablingBluetooth;
  settings_menu_mark_dirty(SettingsMenuItemBluetooth);
}

bool is_remote_connected(StoredRemote* remote) {
  switch (remote->type) {
    case StoredRemoteTypeBTClassic:
      return remote->classic.connected;
    case StoredRemoteTypeBLE:
      return (remote->ble.connection != NULL);
    case StoredRemoteTypeBTDual:
      return remote->dual.classic.connected || (remote->dual.ble.connection != NULL);
    default:
      WTF;
  }
  return false;
}

static int remote_comparator(StoredRemote* remote, StoredRemote* other) {
  if (is_remote_connected(remote) != is_remote_connected(other)) {
    return is_remote_connected(remote) ? -1 : 1;
  } else {
    return strncmp(remote->name, other->name, sizeof(remote->name));
  }
}

static void add_remote(SettingsBluetoothData* data, StoredRemote* remote) {
  const bool ascending = false;
  data->remote_list_head = list_sorted_add(data->remote_list_head, &remote->list_node,
      (Comparator) remote_comparator, ascending);
}

static StoredRemote* stored_remote_create(void) {
  StoredRemote* remote = task_malloc_check(sizeof(*remote));
  *remote = (StoredRemote){};
  return remote;
}

static void prv_copy_device_name_with_fallback(StoredRemote *remote, const char *name) {
  if (!name || strlen(name) == 0) {
    i18n_get_with_buffer("<Untitled>", remote->name, sizeof(remote->name));
  } else {
    strncpy(remote->name, name, sizeof(remote->name));
  }
}

static void prv_add_bt_classic_remote(BTDeviceAddress *addr, SM128BitKey *link_key,
                                      const char *name, uint8_t *platform_bits, void *context) {
  SettingsBluetoothData *data = (SettingsBluetoothData*) context;
  if (!data) {
    return;
  }

  // Determine the address of our active remote, if we have one.
  BTDeviceAddress active_addr = {};
  const bool is_connected = bt_driver_classic_copy_connected_address(&active_addr);

  // Create the new remote
  StoredRemote *remote = stored_remote_create();
  remote->classic.bd_addr = *addr;
  prv_copy_device_name_with_fallback(remote, name);

  if (is_connected && (0 == memcmp(addr, &active_addr, sizeof(*addr)))) {
    remote->classic.connected = true;
  } else {
    remote->classic.connected = false;
  }

  add_remote(data, remote);
}


static void prv_add_bt_classic_remotes(SettingsBluetoothData *data) {
  bt_persistent_storage_for_each_bt_classic_pairing(prv_add_bt_classic_remote, data);
}

static bool dual_remote_filter(ListNode *node, void *data) {
  StoredRemote *classic_remote = (StoredRemote *) node;
  BTDeviceInternal *device = (BTDeviceInternal *) data;
  BTDeviceInternal le_device_with_classic_address = (const BTDeviceInternal) {
    .address = classic_remote->classic.bd_addr,
    .is_random_address = false,
  };
  return bt_device_equal(&le_device_with_classic_address.opaque, &device->opaque);
}

static void prv_add_and_merge_ble_remote(BTDeviceInternal *device, SMIdentityResolvingKey *irk,
                                         const char *name, BTBondingID *id, void *context) {
  SettingsBluetoothData *data = (SettingsBluetoothData*) context;
  if (!data) {
    return;
  }

  StoredRemote* remote = (StoredRemote*) list_find_next(data->remote_list_head,
                                                        dual_remote_filter, true, device);
  if (remote) {
    // The remote is also a ble device, promote to a dual remote
    const bool classic_connected = remote->classic.connected;
    remote->type = StoredRemoteTypeBTDual;
    remote->dual.classic.connected = classic_connected;
    // Note: We update remote->dual.ble.connected outside this cb
    remote->dual.ble.bonding = *id;
  } else {
    // Remote for which we only have a BLE key, add it in the menu as well, so it is accessible
    // and can be removed by the user:
    StoredRemote* remote = stored_remote_create();
    remote->type = StoredRemoteTypeBLE;
    // Note: We update remote->ble.connection outside this cb
    remote->ble.bonding = *id;
    prv_copy_device_name_with_fallback(remote, name);
    add_remote(data, remote);
  }
}

//! This must be called after updating classic remotes for remote consolidation
static void prv_add_and_merge_ble_remotes(SettingsBluetoothData *data) {
  bt_persistent_storage_for_each_ble_pairing(prv_add_and_merge_ble_remote, data);

  StoredRemote *remote = (StoredRemote *)data->remote_list_head;
  while (remote) {
    StoredRemoteBLE *ble_rem = NULL;
    if (remote->type == StoredRemoteTypeBLE) {
      ble_rem = &remote->ble;
    } else if (remote->type == StoredRemoteTypeBTDual) {
      ble_rem = &remote->dual.ble;
    }

    if (ble_rem) {
      SMIdentityResolvingKey irk;
      BTDeviceInternal device;

      if (bt_persistent_storage_get_ble_pairing_by_id(ble_rem->bonding, &irk, &device, NULL)) {
        bt_lock();
        GAPLEConnection *connection = gap_le_connection_find_by_irk(&irk);
        if (!connection) {
          connection = gap_le_connection_by_device(&device);
        }
        ble_rem->connection = connection;
#if CAPABILITY_HAS_BUILTIN_HRM
        ble_rem->is_sharing_heart_rate = ble_hrm_is_sharing_to_connection(connection);
#endif
        bt_unlock();
      }
    }
    remote = (StoredRemote *)remote->list_node.next;
  }
}

static void prv_clear_remote_list(SettingsBluetoothData* data) {
  while (data->remote_list_head) {
    StoredRemote* remote = (StoredRemote*) data->remote_list_head;
    data->remote_list_head = list_pop_head(&remote->list_node);
    task_free(remote);
  }
}

static void prv_reload_remote_list(SettingsBluetoothData* data) {
  prv_clear_remote_list(data);
  prv_add_bt_classic_remotes(data);
  prv_add_and_merge_ble_remotes(data);
}

static void settings_bluetooth_update_remotes_private(SettingsBluetoothData* data) {
  prv_reload_remote_list(data);

  if (!data->remote_list_head) {
    strncpy(data->header_buffer, i18n_get("Pairing Instructions", data), HEADER_BUFFER_SIZE);
  } else {
    const unsigned int num_remotes = list_count(data->remote_list_head);
    sniprintf(data->header_buffer, HEADER_BUFFER_SIZE,
              (num_remotes != 1) ? i18n_get("%u Paired Phones", data) :
                                   i18n_get("%u Paired Phone", data),
              num_remotes);
  }
}

void settings_bluetooth_update_remotes(SettingsBluetoothData *data) {
  settings_bluetooth_update_remotes_private(data);
  settings_menu_reload_data(SettingsMenuItemBluetooth);
}

//////////

static void prv_settings_bluetooth_event_handler(PebbleEvent *event, void *context) {
  SettingsBluetoothData* settings_data = (SettingsBluetoothData *) context;
  PBL_LOG_COLOR(LOG_LEVEL_DEBUG, LOG_COLOR_BLUE, "BT EVENT");
  switch (event->type) {
    case PEBBLE_BT_CONNECTION_EVENT:
      // If BT Settings is open, update BLE device name upon connecting device:
      if (event->bluetooth.connection.is_ble &&
          event->bluetooth.connection.state == PebbleBluetoothConnectionEventStateConnected) {
        // https://pebbletechnology.atlassian.net/browse/PBL-22176
        // iOS seems to respond with 0x0E (Unlikely Error) when performing this request while
        // the encryption set up is going on. For non-bonded devices it will work fine though.
        gap_le_device_name_request(&event->bluetooth.connection.device);
      }
      // fall-through!
    case PEBBLE_BT_PAIRING_EVENT:
#if CAPABILITY_HAS_BUILTIN_HRM
    case PEBBLE_BLE_HRM_SHARING_STATE_UPDATED_EVENT:
#endif
    case PEBBLE_BLE_DEVICE_NAME_UPDATED_EVENT: {
      settings_bluetooth_update_remotes_private(settings_data);
      settings_menu_mark_dirty(SettingsMenuItemBluetooth);
      break;
    }

    case PEBBLE_BT_STATE_EVENT: {
      settings_bluetooth_reconnect_once();
      settings_data->toggle_state = ToggleStateIdle;
      settings_menu_mark_dirty(SettingsMenuItemBluetooth);
      break;
    }

    default:
      break;
  }
}

// UI Stuff
/////////////////////////////
// Menu Layer Callbacks
/////////////////////////////
//-- Address
//   ...
//|  Airplane Mode: Off
//-- Paired Devices
//|  Device Name
//   Connected
//|  Device Name
//

static void prv_draw_stored_remote_item_rect(GContext *ctx, const Layer *cell_layer,
                                             const char *remote_name, const char *connected_string,
                                             const char *le_string,
                                             const char *is_sharing_heart_rate_string) {
  const GFont font = ((le_string || is_sharing_heart_rate_string) ?
                      fonts_get_system_font(FONT_KEY_GOTHIC_18) : NULL);

  if (le_string) {
    GRect box = cell_layer->bounds;
    box.size.w -= 5;
    box.origin.y += 20;
    box.size.h = 24;

    graphics_draw_text(ctx, le_string, font, box, GTextOverflowModeFill, GTextAlignmentRight, NULL);
  }

  if (is_sharing_heart_rate_string) {
    const int horizontal_margin = menu_cell_basic_horizontal_inset();
    GRect box = grect_inset(cell_layer->bounds, GEdgeInsets(0, horizontal_margin));
    box.origin.y += 38;
    box.size.h = 24;

    graphics_draw_text(ctx, is_sharing_heart_rate_string, font, box,
                       GTextOverflowModeFill, GTextAlignmentLeft, NULL);

    // Gross hack to avoid centering the title / subtitle labels in the entire cell:
    ((Layer *)cell_layer)->bounds.size.h -= SHARING_HEART_RATE_EXTRA_HEIGHT_PX;
  }

  menu_cell_basic_draw(ctx, cell_layer, remote_name, connected_string, NULL);

  if (is_sharing_heart_rate_string) {
    // Restore original height:
    ((Layer *)cell_layer)->bounds.size.h += SHARING_HEART_RATE_EXTRA_HEIGHT_PX;
  }
}

bool settings_bluetooth_is_sharing_heart_rate_for_stored_remote(StoredRemote* remote) {
#if CAPABILITY_HAS_BUILTIN_HRM
  switch (remote->type) {
    case StoredRemoteTypeBLE: return remote->ble.is_sharing_heart_rate;
    case StoredRemoteTypeBTDual: return remote->dual.ble.is_sharing_heart_rate;
    default:
      return false;
  }
#else
  return false;
#endif  // CAPABILITY_HAS_BUILTIN_HRM
}

#if PBL_ROUND
static void prv_draw_stored_remote_item_round(GContext *ctx, const Layer *cell_layer,
                                              const char *remote_name, const char *connected_string,
                                              const char *le_string,
                                              const char *is_sharing_heart_rate_string) {
#  if CAPABILITY_HAS_BUILTIN_HRM
  _Static_assert(false, "FIXME: Implement round drawing code to show heart rate sharing status!");
#  endif  // CAPABILITY_HAS_BUILTIN_HRM
  menu_cell_basic_draw(ctx, cell_layer, remote_name, connected_string, NULL);
}
#endif  // PBL_ROUND

static void draw_stored_remote_item(GContext *ctx, const Layer *cell_layer,
                                    uint16_t device_index, SettingsBluetoothData *data) {
  const uint32_t num_remotes = list_count(data->remote_list_head);
  PBL_ASSERT(device_index < num_remotes, "Got index %" PRId16 " only have %" PRId32,
             device_index, num_remotes);
  StoredRemote* remote = (StoredRemote*) list_get_at(data->remote_list_head, device_index);
  bool connected = is_remote_connected(remote);

  const char *le_string = NULL;
  if (remote->type == StoredRemoteTypeBTDual
      && remote->dual.classic.connected != (remote->dual.ble.connection != NULL)) {
    le_string = remote->dual.classic.connected
        ? i18n_get("No LE", data) : i18n_get("LE Only", data);
  }

  const char *connected_string = connected ? i18n_get("Connected", data) :
                                 PBL_IF_RECT_ELSE("", NULL);

  // Add ellipsis if the name might have been cut off by the mobile
  const char ellipsis[] = UTF8_ELLIPSIS_STRING;
  const size_t max_name_size = BT_DEVICE_NAME_BUFFER_SIZE - 2;
  const size_t name_size = strnlen(remote->name, BT_DEVICE_NAME_BUFFER_SIZE);
  char *remote_name = task_zalloc_check(max_name_size + sizeof(ellipsis));
  strncpy(remote_name, remote->name, name_size);
  if (name_size > max_name_size) {
    const size_t ellipsis_start_offset = utf8_get_size_truncate(remote_name, name_size);
    strncpy(&remote_name[ellipsis_start_offset], ellipsis, sizeof(ellipsis));
  }

  const char *is_sharing_heart_rate =
      (settings_bluetooth_is_sharing_heart_rate_for_stored_remote(remote) ?
       i18n_get("Sharing Heart Rate ❤", data) : NULL);

  PBL_IF_RECT_ELSE(prv_draw_stored_remote_item_rect,
                   prv_draw_stored_remote_item_round)(ctx, cell_layer, remote_name,
                                                      connected_string, le_string,
                                                      is_sharing_heart_rate);

  task_free(remote_name);
}


static uint16_t prv_num_rows_cb(SettingsCallbacks *context) {
  SettingsBluetoothData *data = (SettingsBluetoothData *) context;
  return list_count(data->remote_list_head) + 1;
}

static int16_t prv_row_height_cb(SettingsCallbacks *context, uint16_t row, bool is_selected) {
#if PBL_RECT
#  if CAPABILITY_HAS_BUILTIN_HRM
  int heart_rate_sharing_text_height = 0;
  if (row > 0) {
    SettingsBluetoothData *data = (SettingsBluetoothData *) context;
    const uint16_t device_index = row - 1;
    StoredRemote* remote = (StoredRemote*) list_get_at(data->remote_list_head, device_index);
    if (settings_bluetooth_is_sharing_heart_rate_for_stored_remote(remote)) {
      heart_rate_sharing_text_height = SHARING_HEART_RATE_EXTRA_HEIGHT_PX;
    }
  }
#  else
  const int heart_rate_sharing_text_height = 0;
#  endif  // CAPABILITY_HAS_BUILTIN_HRM
  return menu_cell_basic_cell_height() + heart_rate_sharing_text_height;
#elif PBL_ROUND
  return (is_selected ? MENU_CELL_ROUND_FOCUSED_TALL_CELL_HEIGHT :
          MENU_CELL_ROUND_UNFOCUSED_SHORT_CELL_HEIGHT);
#else
#endif
}

static void prv_draw_row_cb(SettingsCallbacks *context, GContext *ctx,
                            const Layer *cell_layer, uint16_t row, bool selected) {
  SettingsBluetoothData *data = (SettingsBluetoothData *) context;
  if (row == 0) {
      char device_name_buffer[BT_DEVICE_NAME_BUFFER_SIZE];
      const char *subtitle = NULL;
      const char *title = i18n_get("Connection", data);
      GBitmap *icon = NULL;
      if (data->toggle_state == ToggleStateIdle) {
        if (bt_ctl_is_airplane_mode_on()) {
          subtitle = i18n_get("Airplane Mode", data);
          icon = &data->icon_heap_bitmap[AirplaneIconIdx];
        } else {
          if (selected) {
            bt_local_id_copy_device_name(device_name_buffer, false);
            subtitle = device_name_buffer;
          } else {
            subtitle = i18n_get("Now Discoverable", data);
          }
          icon = &data->icon_heap_bitmap[BluetoothIconIdx];
        }
      } else {
        subtitle = (data->toggle_state == ToggleStateDisablingBluetooth)
            ? i18n_get("Disabling...", data) : i18n_get("Enabling...", data);
        icon = &data->icon_heap_bitmap[BluetoothAltIconIdx];
      }

      menu_cell_basic_draw(ctx, cell_layer, title, subtitle, icon);

    // TODO PBL-23111: Decide how we should show these strings on round displays
#if PBL_RECT
      // Hack: the pairing instruction is drawn in the cell callback, but outside of the cell...
      if (!data->remote_list_head) {
        const GDrawState draw_state = ctx->draw_state;
        // Enable drawing outside of the cell:
        ctx->draw_state.clip_box = ctx->dest_bitmap.bounds;

        graphics_context_set_text_color(ctx, GColorBlack);
        GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_18);
        GRect box = cell_layer->bounds;
        box.origin.x = 15;
        box.origin.y = menu_cell_basic_cell_height() + (int16_t)9;
        box.size.w -= 30;
        box.size.h = 83;

        if (bt_ctl_is_airplane_mode_on()) {
          graphics_draw_text(ctx, i18n_get("Disable Airplane Mode to connect.", data), font,
                             box, GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
        } else {
          graphics_draw_text(ctx, i18n_get("Open the Pebble app on your phone to connect.", data),
                             font, box, GTextOverflowModeTrailingEllipsis,
                             GTextAlignmentCenter, NULL);
        }

        ctx->draw_state = draw_state;
      }
#endif
  } else {
    const uint16_t device_index = row - 1;
    draw_stored_remote_item(ctx, cell_layer, device_index, data);
  }
}

static void prv_select_click_cb(SettingsCallbacks *context, uint16_t row) {
  SettingsBluetoothData *data = (SettingsBluetoothData *) context;
  if (row == 0) {
    settings_bluetooth_toggle_airplane_mode(data);
    return;
  }
  if (!data->remote_list_head) {
    return;
  }
  prv_reload_remote_list(data);
  StoredRemote* remote = (StoredRemote*) list_get_at(data->remote_list_head, row - 1);
  settings_remote_menu_push(data, remote);
}

static void prv_focus_handler(bool in_focus) {
  if (!in_focus) {
    return;
  }
  settings_menu_reload_data(SettingsMenuItemBluetooth);
}

static void prv_expand_cb(SettingsCallbacks *context) {
  SettingsBluetoothData *data = (SettingsBluetoothData *) context;

  settings_bluetooth_update_remotes_private(data);

  // When entering the BT Settings, update device names of all connected devices:
  if (!bt_ctl_is_airplane_mode_on()) {
    gap_le_device_name_request_all();
  }

  data->bt_airplane_event_info = (EventServiceInfo) {
    .type = PEBBLE_BT_STATE_EVENT,
    .handler = prv_settings_bluetooth_event_handler,
    .context = data,
  };
  data->bt_connection_event_info = (EventServiceInfo) {
    .type = PEBBLE_BT_CONNECTION_EVENT,
    .handler = prv_settings_bluetooth_event_handler,
    .context = data,
  };
  data->bt_pairing_event_info = (EventServiceInfo) {
    .type = PEBBLE_BT_PAIRING_EVENT,
    .handler = prv_settings_bluetooth_event_handler,
    .context = data,
  };
  data->ble_device_name_updated_event_info = (EventServiceInfo) {
    .type = PEBBLE_BLE_DEVICE_NAME_UPDATED_EVENT,
    .handler = prv_settings_bluetooth_event_handler,
    .context = data,
  };
#if CAPABILITY_HAS_BUILTIN_HRM
  data->ble_hrm_sharing_event_info = (EventServiceInfo) {
    .type = PEBBLE_BLE_HRM_SHARING_STATE_UPDATED_EVENT,
    .handler = prv_settings_bluetooth_event_handler,
    .context = data,
  };
  event_service_client_subscribe(&data->ble_hrm_sharing_event_info);
#endif
  event_service_client_subscribe(&data->bt_airplane_event_info);
  event_service_client_subscribe(&data->bt_connection_event_info);
  event_service_client_subscribe(&data->bt_pairing_event_info);
  event_service_client_subscribe(&data->ble_device_name_updated_event_info);
  // FIXME: We do not support multiple BLE+ANCS bondings yet, so do not enable pairability if
  // we have an existing one.
  // NOTE: All BLE bondings are considered to be BLE+ANCS now!
  if (!bt_persistent_storage_has_ble_ancs_bonding()) {
    bt_pairability_use();
    data->pairable = true;
  }
  bt_driver_reconnect_pause();
  // Reload & redraw after pairing popup
  app_focus_service_subscribe_handlers((AppFocusHandlers) { .did_focus = prv_focus_handler });
}

// Turns off services that are part of the bluetooth settings menu such as enabling
// discovery. We don't want to keep these services running longer than necessary because
// they consume a fair amount of power
static void prv_hide_cb(SettingsCallbacks *context) {
  SettingsBluetoothData *data = (SettingsBluetoothData *) context;
  if (data->pairable) {
    bt_pairability_release();
  }
  bt_driver_reconnect_resume();
  bt_driver_reconnect_reset_interval();
  bt_driver_reconnect_try_now(false /*ignore_paused*/);

#if CAPABILITY_HAS_BUILTIN_HRM
  event_service_client_unsubscribe(&data->ble_hrm_sharing_event_info);
#endif
  event_service_client_unsubscribe(&data->bt_airplane_event_info);
  event_service_client_unsubscribe(&data->bt_connection_event_info);
  event_service_client_unsubscribe(&data->bt_pairing_event_info);
  event_service_client_unsubscribe(&data->ble_device_name_updated_event_info);
  app_focus_service_unsubscribe();
}

static void prv_deinit_cb(SettingsCallbacks *context) {
  SettingsBluetoothData *data = (SettingsBluetoothData *) context;

  i18n_free_all(data);

  prv_clear_remote_list(data);
  for (unsigned int idx = 0; idx < NumIcons; ++idx) {
    gbitmap_deinit(&data->icon_heap_bitmap[idx]);
  }
  app_free(data);
}

static Window *prv_init(void) {
  SettingsBluetoothData *data = app_malloc_check(sizeof(SettingsBluetoothData));
  *data = (SettingsBluetoothData){};

  for (unsigned int idx = 0; idx < NumIcons; ++idx) {
    gbitmap_init_with_resource(&data->icon_heap_bitmap[idx], ICON_RESOURCE_ID[idx]);
  }

  data->callbacks = (SettingsCallbacks) {
    .deinit = prv_deinit_cb,
    .draw_row = prv_draw_row_cb,
    .select_click = prv_select_click_cb,
    .num_rows = prv_num_rows_cb,
    .row_height = prv_row_height_cb,
    .expand = prv_expand_cb,
    .hide = prv_hide_cb,
  };

  return settings_window_create(SettingsMenuItemBluetooth, &data->callbacks);
}

const SettingsModuleMetadata *settings_bluetooth_get_info(void) {
  static const SettingsModuleMetadata s_module_info = {
    .name = i18n_noop("Bluetooth"),
    .init = prv_init,
  };

  return &s_module_info;
}

#undef HEADER_BUFFER_SIZE
