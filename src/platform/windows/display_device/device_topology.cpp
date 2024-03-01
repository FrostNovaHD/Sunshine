// lib includes
#include <boost/variant.hpp>

// local includes
#include "src/logging.h"
#include "windows_utils.h"

namespace display_device {

  namespace {

    using device_topology_map_t = std::unordered_map<std::string, std::size_t>;

    /*!
     * Parses the path into a map of ["valid" device id -> path index].
     *
     * The paths are already ordered as the "best in front". This includes both
     * inactive and active devices. What's important is to select only device
     * per a device path (which we use for our device id in this case).
     *
     * There can be multiple valid paths per an adapter, but we only care
     * about the "best one" - the first in the list.
     *
     * From experimentation it seems that it does not really matter for Windows
     * if you select not the "best" valid path as it will just ignore your selection
     * and still select the "best" path anyway. This can be verified by looking at the
     * source ids from the structure and how they do not match the ones from the paths
     * you give to Windows (unless they are not persistent or generated on the fly).
     */
    device_topology_map_t
    make_valid_topology_map(const std::vector<DISPLAYCONFIG_PATH_INFO> &paths) {
      device_topology_map_t current_topology;
      std::unordered_set<std::string> used_paths;
      for (std::size_t index = 0; index < paths.size(); ++index) {
        const auto &path { paths[index] };

        const auto device_info { w_utils::get_device_info_for_valid_path(path, w_utils::ALL_DEVICES) };
        if (!device_info) {
          // Path is not valid
          continue;
        }

        if (used_paths.count(device_info->device_path) > 0) {
          // Path was already selected
          continue;
        }

        if (current_topology.count(device_info->device_id) > 0) {
          BOOST_LOG(error) << "duplicate display device id found: " << device_info->device_id;
          return {};
        }

        used_paths.insert(device_info->device_path);
        current_topology[device_info->device_id] = index;
        BOOST_LOG(verbose) << "new valid topology entry [" << index << "] for device " << device_info->device_id << " (device path: " << device_info->device_path << ")";
      }

      return current_topology;
    }

    struct preexisting_topology_t {
      UINT32 group_id;
      std::size_t path_index;
    };
    using preexisting_topology_map_t = std::unordered_map<std::string, preexisting_topology_t>;

    /*!
     * Here we try to generate a topology that we expect Windows to already know about and have settings.
     * Devices that we want to have duplicated shall have the same group id and devices that we want
     * to have extended shall have different group ids.
     *
     * Group id is just and arbitrary number that does have to be continuous.
     */
    preexisting_topology_map_t
    make_preexisting_topology(const active_topology_t &new_topology, const device_topology_map_t &current_topology) {
      UINT32 group_id { 0 };
      preexisting_topology_map_t preexisting_topology;

      for (const auto &group : new_topology) {
        for (const std::string &device_id : group) {
          auto device_topology_it { current_topology.find(device_id) };
          if (device_topology_it == std::end(current_topology)) {
            BOOST_LOG(error) << "device " << device_id << " does not exist in the current topology!";
            return {};
          }

          const auto path_index { device_topology_it->second };
          preexisting_topology[device_id] = { group_id, path_index };
        }

        group_id++;
      }

      return preexisting_topology;
    }

    struct updated_topology_t {
      boost::variant<UINT32, DISPLAYCONFIG_PATH_INFO> group_id_or_path;
      std::size_t path_index;
    };
    using updated_topology_info_map_t = std::unordered_map<std::string, updated_topology_t>;

    /*!
     * Similar to "make_preexisting_topology", the only difference is that we try
     * to preserve information from active paths.
     */
    updated_topology_info_map_t
    make_updated_topology(const active_topology_t &new_topology, const device_topology_map_t &current_topology, const std::vector<DISPLAYCONFIG_PATH_INFO> &paths) {
      UINT32 group_id { 0 };
      std::unordered_map<std::string, std::unordered_set<UINT32>> taken_source_ids;
      updated_topology_info_map_t updated_topology;

      for (const auto &group : new_topology) {
        int inactive_devices_per_group { 0 };

        // First we want to save path info from already active devices
        for (const std::string &device_id : group) {
          auto device_topology_it { current_topology.find(device_id) };
          if (device_topology_it == std::end(current_topology)) {
            BOOST_LOG(error) << "device " << device_id << " does not exist in the current topology!";
            return {};
          }

          const auto path_index { device_topology_it->second };
          if (!w_utils::is_active(paths.at(path_index))) {
            continue;
          }

          updated_topology[device_id] = { paths[path_index], path_index };
        }

        // Next we want to assign new groups for inactive devices only
        for (const std::string &device_id : group) {
          auto device_topology_it { current_topology.find(device_id) };
          if (device_topology_it == std::end(current_topology)) {
            BOOST_LOG(error) << "device " << device_id << " does not exist in the current topology!";
            return {};
          }

          const auto path_index { device_topology_it->second };
          if (w_utils::is_active(paths.at(path_index))) {
            continue;
          }

          updated_topology[device_id] = { group_id, path_index };
          inactive_devices_per_group++;
        }

        // In case we have duplicated displays with inactive devices we now want to discard the active device path infomation
        // completely and let Windows take care of it. We just need to make sure they have the same group id.
        if (inactive_devices_per_group > 1) {
          for (const std::string &device_id : group) {
            auto info_it { updated_topology.find(device_id) };
            if (info_it == std::end(updated_topology)) {
              // Sanity check
              BOOST_LOG(error) << "device " << device_id << " does not exist in the updated topology!";
              return {};
            }

            info_it->second.group_id_or_path = group_id;
          }
        }

        group_id++;
      }

      return updated_topology;
    }

    /*!
     * Try to set to the new topology.
     * Either by trying to reuse preexisting ones or creating a new
     * topology that Windows has never seen before.
     */
    bool
    do_set_topology(const active_topology_t &new_topology) {
      auto display_data { w_utils::query_display_config(w_utils::ALL_DEVICES) };
      if (!display_data) {
        // Error already logged
        return false;
      }

      const auto current_topology { make_valid_topology_map(display_data->paths) };
      if (current_topology.empty()) {
        // Error already logged
        return false;
      }

      const auto clear_path_data = [&]() {
        // These fields need to be cleared (according to MSDOCS) for devices we want to deactivate or modify.
        // When modifying, we will restore some of them.
        for (auto &path : display_data->paths) {
          w_utils::set_source_index(path, boost::none);
          w_utils::set_target_index(path, boost::none);
          w_utils::set_desktop_index(path, boost::none);
          w_utils::set_clone_group_id(path, boost::none);
          w_utils::set_inactive(path);
          w_utils::clear_path_refresh_rate(path);
        }
      };

      // Try to reuse existing topology settings from Windows first
      {
        const auto preexisting_topology { make_preexisting_topology(new_topology, current_topology) };
        if (preexisting_topology.empty()) {
          BOOST_LOG(error) << "could not make preexisting topology info!";
          return false;
        }

        clear_path_data();
        for (const auto &topology : preexisting_topology) {
          const auto &info { topology.second };
          auto &path { display_data->paths.at(info.path_index) };

          // For Windows to find existing topology we need to only set the group id and mark the device as active
          w_utils::set_clone_group_id(path, info.group_id);
          w_utils::set_active(path);
        }

        const UINT32 validate_flags { SDC_VALIDATE | SDC_TOPOLOGY_SUPPLIED | SDC_ALLOW_PATH_ORDER_CHANGES | SDC_VIRTUAL_MODE_AWARE };
        const UINT32 apply_flags { SDC_APPLY | SDC_TOPOLOGY_SUPPLIED | SDC_ALLOW_PATH_ORDER_CHANGES | SDC_VIRTUAL_MODE_AWARE };

        LONG result { SetDisplayConfig(display_data->paths.size(), display_data->paths.data(), 0, nullptr, validate_flags) };
        if (result == ERROR_SUCCESS) {
          result = SetDisplayConfig(display_data->paths.size(), display_data->paths.data(), 0, nullptr, apply_flags);
          if (result != ERROR_SUCCESS) {
            BOOST_LOG(error) << w_utils::get_ccd_error_string(result) << " failed to change topology using supplied topology!";
            return false;
          }
          else {
            return true;
          }
        }
        else {
          BOOST_LOG(info) << w_utils::get_ccd_error_string(result) << " failed to change topology using supplied topology! Trying to update topology next.";
        }
      }

      // Try to create new/updated topology and save it to the Windows' database
      const auto updated_topology_info { make_updated_topology(new_topology, current_topology, display_data->paths) };
      if (updated_topology_info.empty()) {
        BOOST_LOG(error) << "could not make updated topology info!";
        return false;
      }

      clear_path_data();
      for (const auto &updated_topology : updated_topology_info) {
        const auto &info { updated_topology.second };
        auto &path { display_data->paths[info.path_index] };

        if (const UINT32 *group_id = boost::get<UINT32>(&info.group_id_or_path)) {
          // Same as when trying to reuse topology - let Windows handle it. Specify
          // the group id which indicates just that + mark the device as active
          w_utils::set_clone_group_id(path, *group_id);
          w_utils::set_active(path);
        }
        else if (const auto *saved_path = boost::get<DISPLAYCONFIG_PATH_INFO>(&info.group_id_or_path)) {
          // The device will not be duplicated so let's just preserve the path data
          // from the current topology
          path = *saved_path;
        }
      }

      const UINT32 apply_flags { SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_VIRTUAL_MODE_AWARE | SDC_SAVE_TO_DATABASE };
      const LONG result { SetDisplayConfig(display_data->paths.size(), display_data->paths.data(), display_data->modes.size(), display_data->modes.data(), apply_flags) };
      if (result != ERROR_SUCCESS) {
        BOOST_LOG(error) << w_utils::get_ccd_error_string(result) << " failed to change topology using supplied display config!";
        return false;
      }

      return true;
    }

  }  // namespace

  device_info_map_t
  enum_available_devices() {
    auto display_data { w_utils::query_display_config(w_utils::ALL_DEVICES) };
    if (!display_data) {
      // Error already logged
      return {};
    }

    device_info_map_t available_devices;
    const auto current_topology { make_valid_topology_map(display_data->paths) };
    if (current_topology.empty()) {
      // Error already logged
      return {};
    }

    for (const auto &topology : current_topology) {
      const auto &device_id { topology.first };
      const auto path_index { topology.second };
      const auto &path { display_data->paths.at(path_index) };

      if (w_utils::is_active(path)) {
        const auto mode { w_utils::get_source_mode(w_utils::get_source_index(path, display_data->modes), display_data->modes) };

        available_devices[device_id] = device_info_t {
          w_utils::get_display_name(path),
          w_utils::get_friendly_name(path),
          mode && w_utils::is_primary(*mode) ? device_state_e::primary : device_state_e::active,
          w_utils::get_hdr_state(path)
        };
      }
      else {
        available_devices[device_id] = device_info_t {
          std::string {},  // Inactive device can have multiple display names, so it's just meaningless
          w_utils::get_friendly_name(path),
          device_state_e::inactive,
          hdr_state_e::unknown
        };
      }
    }

    return available_devices;
  }

  active_topology_t
  get_current_topology() {
    const auto display_data { w_utils::query_display_config(w_utils::ACTIVE_ONLY_DEVICES) };
    if (!display_data) {
      // Error already logged
      return {};
    }

    // Duplicate displays can be identified by having the same x/y position. Here we have have a
    // "position to index" for a simple and lazy lookup in case we have to add a device to the
    // topology group.
    std::unordered_map<std::string, std::size_t> position_to_topology_index;
    active_topology_t topology;
    for (const auto &path : display_data->paths) {
      const auto device_info { w_utils::get_device_info_for_valid_path(path, w_utils::ACTIVE_ONLY_DEVICES) };
      if (!device_info) {
        continue;
      }

      const auto source_mode { w_utils::get_source_mode(w_utils::get_source_index(path, display_data->modes), display_data->modes) };
      if (!source_mode) {
        BOOST_LOG(error) << "active device does not have a source mode: " << device_info->device_id << "!";
        return {};
      }

      const std::string lazy_lookup { std::to_string(source_mode->position.x) + std::to_string(source_mode->position.y) };
      auto index_it { position_to_topology_index.find(lazy_lookup) };

      if (index_it == std::end(position_to_topology_index)) {
        position_to_topology_index[lazy_lookup] = topology.size();
        topology.push_back({ device_info->device_id });
      }
      else {
        topology.at(index_it->second).push_back(device_info->device_id);
      }
    }

    return topology;
  }

  bool
  is_topology_valid(const active_topology_t &topology) {
    if (topology.empty()) {
      BOOST_LOG(warning) << "topology input is empty!";
      return false;
    }

    std::unordered_set<std::string> device_ids;
    for (const auto &group : topology) {
      // Size 2 is a Windows' limitation.
      // You CAN set the group to be more than 2, but then
      // Windows' settings app breaks since it was not designed for this :/
      if (group.empty() || group.size() > 2) {
        BOOST_LOG(warning) << "topology group is invalid!";
        return false;
      }

      for (const auto &device_id : group) {
        if (device_ids.count(device_id) > 0) {
          BOOST_LOG(warning) << "duplicate device ids found!";
          return false;
        }

        device_ids.insert(device_id);
      }
    }

    return true;
  }

  bool
  is_topology_the_same(const active_topology_t &a, const active_topology_t &b) {
    const auto sort_topology = [](active_topology_t &topology) {
      for (auto &group : topology) {
        std::sort(std::begin(group), std::end(group));
      }

      std::sort(std::begin(topology), std::end(topology));
    };

    auto a_copy { a };
    auto b_copy { b };

    // On Windows order does not matter.
    sort_topology(a_copy);
    sort_topology(b_copy);

    return a_copy == b_copy;
  }

  bool
  set_topology(const active_topology_t &new_topology) {
    if (!is_topology_valid(new_topology)) {
      BOOST_LOG(error) << "topology input is invalid!";
      return false;
    }

    const auto current_topology { get_current_topology() };
    if (current_topology.empty()) {
      BOOST_LOG(error) << "failed to get current topology!";
      return false;
    }

    if (is_topology_the_same(current_topology, new_topology)) {
      BOOST_LOG(debug) << "same topology provided.";
      return true;
    }

    if (do_set_topology(new_topology)) {
      const auto updated_topology { get_current_topology() };
      if (!updated_topology.empty()) {
        if (is_topology_the_same(new_topology, updated_topology)) {
          return true;
        }
        else {
          // There is an interesting bug in Windows when you have nearly
          // identical devices, drivers or something. For example, imagine you have:
          //    AM   - Actual Monitor
          //    IDD1 - Virtual display 1
          //    IDD2 - Virtual display 2
          //
          // You can have the following topology:
          //    [[AM, IDD1]]
          // but not this:
          //    [[AM, IDD2]]
          //
          // Windows API will just default to:
          //    [[AM, IDD1]]
          // even if you provide the second variant. Windows API will think
          // it's OK and just return ERROR_SUCCESS in this case and there is
          // nothing you can do. Even the Windows' settings app will not
          // be able to set the desired topology.
          //
          // There seems to be a workaround - you need to make sure the IDD1
          // device is used somewhere else in the topology, like:
          //    [[AM, IDD2], [IDD1]]
          //
          // However, since we have this bug an additional sanity check is needed
          // regardless of what Windows report back to us.
          BOOST_LOG(error) << "failed to change topology due to Windows bug!";
        }
      }
      else {
        BOOST_LOG(error) << "failed to get updated topology!";
      }

      // Revert back to the original topology
      do_set_topology(current_topology);  // Return value does not matter
    }

    return false;
  }

}  // namespace display_device
