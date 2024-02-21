// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/uuid/name_generator_sha1.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>

// standard includes
#include <iostream>
#include <system_error>

// local includes
#include "src/logging.h"
#include "src/platform/windows/misc.h"
#include "src/utility.h"
#include "windows_utils.h"

// Windows includes after "windows.h"
#include <SetupApi.h>

namespace display_device::w_utils {

  namespace {

    /**
     * @see get_monitor_device_path description for more information as this
     *      function is identical except that it returns wide-string instead
     *      of a normal one.
     */
    std::wstring
    get_monitor_device_path_wstr(const DISPLAYCONFIG_PATH_INFO &path) {
      DISPLAYCONFIG_TARGET_DEVICE_NAME target_name = {};
      target_name.header.adapterId = path.targetInfo.adapterId;
      target_name.header.id = path.targetInfo.id;
      target_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
      target_name.header.size = sizeof(target_name);

      LONG result { DisplayConfigGetDeviceInfo(&target_name.header) };
      if (result != ERROR_SUCCESS) {
        BOOST_LOG(error) << get_error_string(result) << " failed to get target device name!";
        return {};
      }

      return std::wstring { target_name.monitorDevicePath };
    }

    /**
     * @brief Helper method for dealing with SetupAPI.
     * @returns True if device interface path was retrieved and is non-empty, false otherwise.
     * @see get_device_id implementation for more context regarding this madness.
     */
    bool
    get_device_interface_detail(HDEVINFO dev_info_handle, SP_DEVICE_INTERFACE_DATA &dev_interface_data, std::wstring &dev_interface_path, SP_DEVINFO_DATA &dev_info_data) {
      DWORD required_size_in_bytes { 0 };
      if (SetupDiGetDeviceInterfaceDetailW(dev_info_handle, &dev_interface_data, nullptr, 0, &required_size_in_bytes, nullptr)) {
        BOOST_LOG(error) << "\"SetupDiGetDeviceInterfaceDetailW\" did not fail, what?!";
        return false;
      }
      else if (required_size_in_bytes <= 0) {
        BOOST_LOG(error) << get_error_string(static_cast<LONG>(GetLastError())) << " \"SetupDiGetDeviceInterfaceDetailW\" failed while getting size.";
        return false;
      }

      std::vector<std::uint8_t> buffer;
      buffer.resize(required_size_in_bytes);

      // This part is just EVIL!
      auto detail_data { reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data()) };
      detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

      if (!SetupDiGetDeviceInterfaceDetailW(dev_info_handle, &dev_interface_data, detail_data, required_size_in_bytes, nullptr, &dev_info_data)) {
        BOOST_LOG(error) << get_error_string(static_cast<LONG>(GetLastError())) << " \"SetupDiGetDeviceInterfaceDetailW\" failed.";
        return false;
      }

      dev_interface_path = std::wstring { detail_data->DevicePath };
      return !dev_interface_path.empty();
    }

    /**
     * @brief Helper method for dealing with SetupAPI.
     * @returns True if instance id was retrieved and is non-empty, false otherwise.
     * @see get_device_id implementation for more context regarding this madness.
     */
    bool
    get_device_instance_id(HDEVINFO dev_info_handle, SP_DEVINFO_DATA &dev_info_data, std::wstring &instance_id) {
      DWORD required_size_in_characters { 0 };
      if (SetupDiGetDeviceInstanceIdW(dev_info_handle, &dev_info_data, nullptr, 0, &required_size_in_characters)) {
        BOOST_LOG(error) << "\"SetupDiGetDeviceInstanceIdW\" did not fail, what?!";
        return false;
      }
      else if (required_size_in_characters <= 0) {
        BOOST_LOG(error) << get_error_string(static_cast<LONG>(GetLastError())) << " \"SetupDiGetDeviceInstanceIdW\" failed while getting size.";
        return false;
      }

      instance_id.resize(required_size_in_characters);
      if (!SetupDiGetDeviceInstanceIdW(dev_info_handle, &dev_info_data, instance_id.data(), instance_id.size(), nullptr)) {
        BOOST_LOG(error) << get_error_string(static_cast<LONG>(GetLastError())) << " \"SetupDiGetDeviceInstanceIdW\" failed.";
        return false;
      }

      return !instance_id.empty();
    }

    /**
     * @brief Helper method for dealing with SetupAPI.
     * @returns True if EDID was retrieved and is non-empty, false otherwise.
     * @see get_device_id implementation for more context regarding this madness.
     */
    bool
    get_device_edid(HDEVINFO dev_info_handle, SP_DEVINFO_DATA &dev_info_data, std::vector<BYTE> &edid) {
      // We could just directly open the registry key as the path is known, but we can also use the this
      HKEY reg_key { SetupDiOpenDevRegKey(dev_info_handle, &dev_info_data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ) };
      if (reg_key == INVALID_HANDLE_VALUE) {
        BOOST_LOG(error) << get_error_string(static_cast<LONG>(GetLastError())) << " \"SetupDiOpenDevRegKey\" failed.";
        return false;
      }

      const auto reg_key_cleanup {
        util::fail_guard([&reg_key]() {
          const auto status { RegCloseKey(reg_key) };
          if (status != ERROR_SUCCESS) {
            BOOST_LOG(error) << get_error_string(status) << " \"RegCloseKey\" failed.";
          }
        })
      };

      DWORD required_size_in_bytes { 0 };
      auto status { RegQueryValueExW(reg_key, L"EDID", nullptr, nullptr, nullptr, &required_size_in_bytes) };
      if (status != ERROR_SUCCESS) {
        BOOST_LOG(error) << get_error_string(status) << " \"RegQueryValueExW\" failed when getting size.";
        return false;
      }

      edid.resize(required_size_in_bytes);

      status = RegQueryValueExW(reg_key, L"EDID", nullptr, nullptr, edid.data(), &required_size_in_bytes);
      if (status != ERROR_SUCCESS) {
        BOOST_LOG(error) << get_error_string(status) << " \"RegQueryValueExW\" failed when getting size.";
        return false;
      }

      return !edid.empty();
    }

  }  // namespace

  std::string
  get_error_string(LONG error_code) {
    std::stringstream error;
    error << "[code: ";
    switch (error_code) {
      case ERROR_INVALID_PARAMETER:
        error << "ERROR_INVALID_PARAMETER";
        break;
      case ERROR_NOT_SUPPORTED:
        error << "ERROR_NOT_SUPPORTED";
        break;
      case ERROR_ACCESS_DENIED:
        error << "ERROR_ACCESS_DENIED";
        break;
      case ERROR_INSUFFICIENT_BUFFER:
        error << "ERROR_INSUFFICIENT_BUFFER";
        break;
      case ERROR_GEN_FAILURE:
        error << "ERROR_GEN_FAILURE";
        break;
      case ERROR_SUCCESS:
        error << "ERROR_SUCCESS";
        break;
      default:
        error << error_code;
        break;
    }
    error << ", message: " << std::system_category().message(static_cast<int>(error_code)) << "]";
    return error.str();
  }

  bool
  is_primary(const DISPLAYCONFIG_SOURCE_MODE &mode) {
    return mode.position.x == 0 && mode.position.y == 0;
  }

  bool
  are_modes_duplicated(const DISPLAYCONFIG_SOURCE_MODE &mode_a, const DISPLAYCONFIG_SOURCE_MODE &mode_b) {
    return mode_a.position.x == mode_b.position.x && mode_a.position.y == mode_b.position.y;
  }

  bool
  is_available(const DISPLAYCONFIG_PATH_INFO &path) {
    return path.targetInfo.targetAvailable == TRUE;
  }

  bool
  is_active(const DISPLAYCONFIG_PATH_INFO &path) {
    return static_cast<bool>(path.flags & DISPLAYCONFIG_PATH_ACTIVE);
  }

  void
  set_active(DISPLAYCONFIG_PATH_INFO &path) {
    path.flags |= DISPLAYCONFIG_PATH_ACTIVE;
  }

  std::string
  get_device_id(const DISPLAYCONFIG_PATH_INFO &path) {
    const auto device_path { get_monitor_device_path_wstr(path) };
    if (device_path.empty()) {
      // Error already logged
      return {};
    }

    static const GUID monitor_guid { 0xe6f07b5f, 0xee97, 0x4a90, { 0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7 } };
    std::vector<BYTE> device_id_data;

    HDEVINFO dev_info_handle { SetupDiGetClassDevsW(&monitor_guid, nullptr, nullptr, DIGCF_DEVICEINTERFACE) };
    if (dev_info_handle) {
      const auto dev_info_handle_cleanup {
        util::fail_guard([&dev_info_handle]() {
          if (!SetupDiDestroyDeviceInfoList(dev_info_handle)) {
            BOOST_LOG(error) << get_error_string(static_cast<LONG>(GetLastError())) << " \"SetupDiDestroyDeviceInfoList\" failed.";
          }
        })
      };

      SP_DEVICE_INTERFACE_DATA dev_interface_data {};
      dev_interface_data.cbSize = sizeof(dev_interface_data);
      for (DWORD monitor_index = 0;; ++monitor_index) {
        if (!SetupDiEnumDeviceInterfaces(dev_info_handle, nullptr, &monitor_guid, monitor_index, &dev_interface_data)) {
          const DWORD error_code { GetLastError() };
          if (error_code == ERROR_NO_MORE_ITEMS) {
            break;
          }

          BOOST_LOG(warning) << get_error_string(static_cast<LONG>(error_code)) << " \"SetupDiEnumDeviceInterfaces\" failed.";
          continue;
        }

        std::wstring dev_interface_path;
        SP_DEVINFO_DATA dev_info_data {};
        dev_info_data.cbSize = sizeof(dev_info_data);
        if (!get_device_interface_detail(dev_info_handle, dev_interface_data, dev_interface_path, dev_info_data)) {
          // Error already logged
          continue;
        }

        if (!boost::iequals(dev_interface_path, device_path)) {
          continue;
        }

        // Instance ID is unique in the system and persists restarts, but not driver re-installs.
        // It looks like this:
        //     DISPLAY\ACI27EC\5&4FD2DE4&5&UID4352 (also used in the device path it seems)
        //                a    b    c    d    e
        //
        //  a) Hardware ID - stable
        //  b) Either a bus number or has something to do with device capabilities - stable
        //  c) Another ID, somehow tied to adapter (not an adapter ID from path object) - stable
        //  d) Some sort of rotating counter thing, changes after driver reinstall - unstable
        //  e) Seems to be the same as a target ID from path, it changes based on GPU port - semi-stable
        //
        // The instance ID also seems to be a part of the registry key (in case some other info is needed in the future):
        //     HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Enum\DISPLAY\ACI27EC\5&4fd2de4&5&UID4352

        std::wstring instance_id;
        if (!get_device_instance_id(dev_info_handle, dev_info_data, instance_id)) {
          // Error already logged
          break;
        }

        if (!get_device_edid(dev_info_handle, dev_info_data, device_id_data)) {
          // Error already logged
          break;
        }

        // We are going to discard the unstable parts of the instance ID and merge the stable parts with the edid buffer (if available)
        auto unstable_part_index = instance_id.find_first_of(L'&', 0);
        if (unstable_part_index != std::wstring::npos) {
          unstable_part_index = instance_id.find_first_of(L'&', unstable_part_index + 1);
        }

        if (unstable_part_index == std::wstring::npos) {
          BOOST_LOG(error) << "failed to split off the stable part from instance id string " << platf::to_utf8(instance_id);
          break;
        }

        auto semi_stable_part_index = instance_id.find_first_of(L'&', unstable_part_index + 1);
        if (semi_stable_part_index == std::wstring::npos) {
          BOOST_LOG(error) << "failed to split off the semi-stable part from instance id string " << platf::to_utf8(instance_id);
          break;
        }

        BOOST_LOG(verbose) << "creating device id for path " << platf::to_utf8(device_path) << " from EDID and instance ID: " << platf::to_utf8({ std::begin(instance_id), std::begin(instance_id) + unstable_part_index }) << platf::to_utf8({ std::begin(instance_id) + semi_stable_part_index, std::end(instance_id) });
        device_id_data.insert(std::end(device_id_data),
          reinterpret_cast<const BYTE *>(instance_id.data()),
          reinterpret_cast<const BYTE *>(instance_id.data() + unstable_part_index));
        device_id_data.insert(std::end(device_id_data),
          reinterpret_cast<const BYTE *>(instance_id.data() + semi_stable_part_index),
          reinterpret_cast<const BYTE *>(instance_id.data() + instance_id.size()));
        break;
      }
    }

    if (device_id_data.empty()) {
      // Using the device path as a fallback, which is always unique, but not as stable as the preferred one
      BOOST_LOG(verbose) << "creating device id from path " << platf::to_utf8(device_path);
      device_id_data.insert(std::end(device_id_data),
        reinterpret_cast<const BYTE *>(device_path.data()),
        reinterpret_cast<const BYTE *>(device_path.data() + device_path.size()));
    }

    static constexpr boost::uuids::uuid ns_id {};  // null namespace = no salt
    const auto boost_uuid { boost::uuids::name_generator_sha1 { ns_id }(device_id_data.data(), device_id_data.size()) };
    return "{" + boost::uuids::to_string(boost_uuid) + "}";
  }

  std::string
  get_monitor_device_path(const DISPLAYCONFIG_PATH_INFO &path) {
    return platf::to_utf8(get_monitor_device_path_wstr(path));
  }

  std::string
  get_friendly_name(const DISPLAYCONFIG_PATH_INFO &path) {
    DISPLAYCONFIG_TARGET_DEVICE_NAME target_name = {};
    target_name.header.adapterId = path.targetInfo.adapterId;
    target_name.header.id = path.targetInfo.id;
    target_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target_name.header.size = sizeof(target_name);

    LONG result { DisplayConfigGetDeviceInfo(&target_name.header) };
    if (result != ERROR_SUCCESS) {
      BOOST_LOG(error) << get_error_string(result) << " failed to get target device name!";
      return {};
    }

    return target_name.flags.friendlyNameFromEdid ? platf::to_utf8(target_name.monitorFriendlyDeviceName) : std::string {};
  }

  std::string
  get_display_name(const DISPLAYCONFIG_PATH_INFO &path) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name = {};
    source_name.header.id = path.sourceInfo.id;
    source_name.header.adapterId = path.sourceInfo.adapterId;
    source_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source_name.header.size = sizeof(source_name);

    LONG result { DisplayConfigGetDeviceInfo(&source_name.header) };
    if (result != ERROR_SUCCESS) {
      BOOST_LOG(error) << get_error_string(result) << " failed to get display name! ";
      return {};
    }

    return platf::to_utf8(source_name.viewGdiDeviceName);
  }

  hdr_state_e
  get_hdr_state(const DISPLAYCONFIG_PATH_INFO &path) {
    if (!is_active(path)) {
      // Checking if active to suppress the error message below.
      return hdr_state_e::unknown;
    }

    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color_info = {};
    color_info.header.adapterId = path.targetInfo.adapterId;
    color_info.header.id = path.targetInfo.id;
    color_info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    color_info.header.size = sizeof(color_info);

    LONG result { DisplayConfigGetDeviceInfo(&color_info.header) };
    if (result != ERROR_SUCCESS) {
      BOOST_LOG(error) << get_error_string(result) << " failed to get advanced color info! ";
      return hdr_state_e::unknown;
    }

    return color_info.advancedColorSupported ? color_info.advancedColorEnabled ? hdr_state_e::enabled : hdr_state_e::disabled : hdr_state_e::unknown;
  }

  bool
  set_hdr_state(const DISPLAYCONFIG_PATH_INFO &path, bool enable) {
    DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE color_state = {};
    color_state.header.adapterId = path.targetInfo.adapterId;
    color_state.header.id = path.targetInfo.id;
    color_state.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
    color_state.header.size = sizeof(color_state);

    color_state.enableAdvancedColor = enable ? 1 : 0;

    LONG result { DisplayConfigSetDeviceInfo(&color_state.header) };
    if (result != ERROR_SUCCESS) {
      BOOST_LOG(error) << get_error_string(result) << " failed to set advanced color info!";
      return false;
    }

    return true;
  }

  boost::optional<UINT32>
  get_source_index(const DISPLAYCONFIG_PATH_INFO &path, const std::vector<DISPLAYCONFIG_MODE_INFO> &modes) {
    UINT32 index {};
    if (path.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) {
      index = path.sourceInfo.sourceModeInfoIdx;
      if (index == DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID) {
        return boost::none;
      }
    }
    else {
      index = path.sourceInfo.modeInfoIdx;
      if (index == DISPLAYCONFIG_PATH_MODE_IDX_INVALID) {
        return boost::none;
      }
    }

    if (index >= modes.size()) {
      BOOST_LOG(error) << "source index " << index << " is out of range " << modes.size();
      return boost::none;
    }

    return index;
  }

  void
  set_source_index(DISPLAYCONFIG_PATH_INFO &path, const boost::optional<UINT32> &index) {
    if (path.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) {
      if (index) {
        path.sourceInfo.sourceModeInfoIdx = *index;
      }
      else {
        path.sourceInfo.sourceModeInfoIdx = DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID;
      }
    }
    else {
      if (index) {
        path.sourceInfo.modeInfoIdx = *index;
      }
      else {
        path.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
      }
    }
  }

  void
  set_target_index(DISPLAYCONFIG_PATH_INFO &path, const boost::optional<UINT32> &index) {
    if (path.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) {
      if (index) {
        path.targetInfo.targetModeInfoIdx = *index;
      }
      else {
        path.targetInfo.targetModeInfoIdx = DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID;
      }
    }
    else {
      if (index) {
        path.targetInfo.modeInfoIdx = *index;
      }
      else {
        path.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
      }
    }
  }

  void
  set_desktop_index(DISPLAYCONFIG_PATH_INFO &path, const boost::optional<UINT32> &index) {
    if (path.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) {
      if (index) {
        path.targetInfo.desktopModeInfoIdx = *index;
      }
      else {
        path.targetInfo.desktopModeInfoIdx = DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
      }
    }
  }

  void
  set_clone_group_id(DISPLAYCONFIG_PATH_INFO &path, const boost::optional<UINT32> &id) {
    if (path.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) {
      if (id) {
        path.sourceInfo.cloneGroupId = *id;
      }
      else {
        path.sourceInfo.cloneGroupId = DISPLAYCONFIG_PATH_CLONE_GROUP_INVALID;
      }
    }
  }

  const DISPLAYCONFIG_SOURCE_MODE *
  get_source_mode(const boost::optional<UINT32> &index, const std::vector<DISPLAYCONFIG_MODE_INFO> &modes) {
    if (!index) {
      return nullptr;
    }

    if (*index >= modes.size()) {
      BOOST_LOG(error) << "source index " << *index << " is out of range " << modes.size();
      return nullptr;
    }

    const auto &mode { modes[*index] };
    if (mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
      BOOST_LOG(error) << "mode at index " << *index << " is not source mode!";
      return nullptr;
    }

    return &mode.sourceMode;
  }

  DISPLAYCONFIG_SOURCE_MODE *
  get_source_mode(const boost::optional<UINT32> &index, std::vector<DISPLAYCONFIG_MODE_INFO> &modes) {
    return const_cast<DISPLAYCONFIG_SOURCE_MODE *>(get_source_mode(index, const_cast<const std::vector<DISPLAYCONFIG_MODE_INFO> &>(modes)));
  }

  boost::optional<device_info_t>
  get_device_info_for_valid_path(const DISPLAYCONFIG_PATH_INFO &path, bool must_be_active) {
    if (!is_available(path)) {
      // Could be transient issue according to MSDOCS (no longer available, but still "active")
      return boost::none;
    }

    if (must_be_active) {
      if (!is_active(path)) {
        return boost::none;
      }
    }

    const auto device_path { get_monitor_device_path(path) };
    if (device_path.empty()) {
      return boost::none;
    }

    const auto device_id { get_device_id(path) };
    if (device_id.empty()) {
      return boost::none;
    }

    const auto display_name { get_display_name(path) };
    if (display_name.empty()) {
      return boost::none;
    }

    return device_info_t { device_path, device_id };
  }

  boost::optional<path_and_mode_data_t>
  query_display_config(bool active_only) {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG result = ERROR_SUCCESS;

    // When we want to enable/disable displays, we need to get all paths as they will not be active.
    // This will require some additional filtering of duplicate and otherwise useless paths.
    UINT32 flags = active_only ? QDC_ONLY_ACTIVE_PATHS : QDC_ALL_PATHS;
    flags |= QDC_VIRTUAL_MODE_AWARE;  // supported from W10 onwards

    do {
      UINT32 path_count { 0 };
      UINT32 mode_count { 0 };

      result = GetDisplayConfigBufferSizes(flags, &path_count, &mode_count);
      if (result != ERROR_SUCCESS) {
        BOOST_LOG(error) << get_error_string(result) << " failed to get display paths and modes!";
        return boost::none;
      }

      paths.resize(path_count);
      modes.resize(mode_count);
      result = QueryDisplayConfig(flags, &path_count, paths.data(), &mode_count, modes.data(), nullptr);

      // The function may have returned fewer paths/modes than estimated
      paths.resize(path_count);
      modes.resize(mode_count);

      // It's possible that between the call to GetDisplayConfigBufferSizes and QueryDisplayConfig
      // that the display state changed, so loop on the case of ERROR_INSUFFICIENT_BUFFER.
    } while (result == ERROR_INSUFFICIENT_BUFFER);

    if (result != ERROR_SUCCESS) {
      BOOST_LOG(error) << get_error_string(result) << " failed to query display paths and modes!";
      return boost::none;
    }

    return path_and_mode_data_t { paths, modes };
  }

  const DISPLAYCONFIG_PATH_INFO *
  get_active_path(const std::string &device_id, const std::vector<DISPLAYCONFIG_PATH_INFO> &paths) {
    for (const auto &path : paths) {
      const auto device_info { get_device_info_for_valid_path(path, ACTIVE_ONLY_DEVICES) };
      if (!device_info) {
        continue;
      }

      if (device_info->device_id == device_id) {
        return &path;
      }
    }

    return nullptr;
  }

  DISPLAYCONFIG_PATH_INFO *
  get_active_path(const std::string &device_id, std::vector<DISPLAYCONFIG_PATH_INFO> &paths) {
    return const_cast<DISPLAYCONFIG_PATH_INFO *>(get_active_path(device_id, const_cast<const std::vector<DISPLAYCONFIG_PATH_INFO> &>(paths)));
  }

}  // namespace display_device::w_utils
