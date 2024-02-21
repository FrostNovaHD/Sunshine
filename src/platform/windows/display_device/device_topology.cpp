// lib includes
#include <boost/variant.hpp>

// local includes
#include "src/logging.h"
#include "windows_utils.h"

namespace display_device {

  namespace {

    bool
    compareAdapterIds(const LUID &a, const LUID &b) {
      return a.HighPart == b.HighPart && a.LowPart == b.LowPart;
    }

    std::string
    to_string(const LUID &id) {
      return std::to_string(id.HighPart) + std::to_string(id.LowPart);
    }

    struct device_topology_data_t {
      std::unordered_map<UINT32, std::size_t> source_id_to_path_index;
      LUID source_adapter_id;
      boost::optional<UINT32> active_source;

      std::size_t
      get_best_path_index() const {
        if (active_source) {
          return source_id_to_path_index.at(*active_source);
        }

        // All paths are inactive so any will do
        return source_id_to_path_index.at(0);
      }
    };

    using device_topology_data_map_t = std::unordered_map<std::string, device_topology_data_t>;

    /*!
     * Parses the paths into a map of [valid device id -> data that can actually be used].
     */
    device_topology_data_map_t
    make_device_topology_data(const std::vector<DISPLAYCONFIG_PATH_INFO> &paths) {
      device_topology_data_map_t topology_data;
      std::unordered_map<std::string, std::string> paths_to_ids;
      for (std::size_t index = 0; index < paths.size(); ++index) {
        const auto &path { paths[index] };

        const auto device_info { w_utils::get_device_info_for_valid_path(path, w_utils::ALL_DEVICES) };
        if (!device_info) {
          // Path is not valid
          continue;
        }

        const auto prev_device_id_for_path_it { paths_to_ids.find(device_info->device_path) };
        if (prev_device_id_for_path_it != std::end(paths_to_ids)) {
          if (prev_device_id_for_path_it->second != device_info->device_id) {
            BOOST_LOG(error) << "duplicate display device id found: " << device_info->device_id << " (device path: " << device_info->device_path << ")";
            return {};
          }
        }
        else {
          BOOST_LOG(verbose) << "new valid device id entry for device " << device_info->device_id << " (device path: " << device_info->device_path << ")";
          paths_to_ids[device_info->device_path] = device_info->device_id;
        }

        auto topology_data_it { topology_data.find(device_info->device_id) };
        if (topology_data_it != std::end(topology_data)) {
          if (!compareAdapterIds(topology_data_it->second.source_adapter_id, path.sourceInfo.adapterId)) {
            // Sanity check, should not be possible since adapter in embedded in the path
            BOOST_LOG(error) << "device path " << device_info->device_path << " has different adapters!";
            return {};
          }

          topology_data_it->second.source_id_to_path_index[path.sourceInfo.id] = index;
        }
        else {
          topology_data[device_info->device_id] = device_topology_data_t {
            { { path.sourceInfo.id, index } },
            path.sourceInfo.adapterId,
            // Since active paths are always in the front, this is the only time we check (when we add new entry)
            w_utils::is_active(path) ? boost::make_optional(path.sourceInfo.id) : boost::none
          };
        }
      }

      return topology_data;
    }

    /*!
     * Selects the best possible paths for the requested topology based on the data that is available to us.
     *
     * If the paths will be used for a completely new topology (Windows never had it set), we need to take into
     * account the source id availability per the adapter - duplicated displays must share the same source id
     * (if they belong to the same adapter) and have different ids if they are not duplicated displays.
     *
     * There are limited amount of available ids (see comments in the code) so we will abort early if we are
     * out of ids.
     *
     * The paths for a topology that already exists (Windows has set it at least one) does not have to follow
     * the mentioned "source id" rule. Windows will simply ignore them (since we ask it to) and select
     * paths that were previously configured (that might differ in source ids) based on the paths that we provide.
     */
    std::vector<DISPLAYCONFIG_PATH_INFO>
    make_new_paths_for_topology(const active_topology_t &new_topology, const device_topology_data_map_t &topology_data, const std::vector<DISPLAYCONFIG_PATH_INFO> &paths) {
      std::vector<DISPLAYCONFIG_PATH_INFO> new_paths;

      UINT32 group_id { 0 };
      std::unordered_map<std::string, std::unordered_set<UINT32>> used_source_adapter_ids_per_adapter;
      const auto is_source_id_already_used = [&used_source_adapter_ids_per_adapter](const LUID &adapter_id, UINT32 source_id) {
        auto entry_it { used_source_adapter_ids_per_adapter.find(to_string(adapter_id)) };
        if (entry_it != std::end(used_source_adapter_ids_per_adapter)) {
          return entry_it->second.count(source_id) > 0;
        }

        return false;
      };

      for (const auto &group : new_topology) {
        std::unordered_map<std::string, UINT32> used_source_adapter_ids_per_adapter_for_group;
        const auto get_already_used_source_id_in_group = [&used_source_adapter_ids_per_adapter_for_group](const LUID &adapter_id) -> boost::optional<UINT32> {
          auto entry_it { used_source_adapter_ids_per_adapter_for_group.find(to_string(adapter_id)) };
          if (entry_it != std::end(used_source_adapter_ids_per_adapter_for_group)) {
            return entry_it->second;
          }

          return boost::none;
        };

        for (const std::string &device_id : group) {
          auto topology_data_it { topology_data.find(device_id) };
          if (topology_data_it == std::end(topology_data)) {
            BOOST_LOG(error) << "device " << device_id << " does not exist in the available topology data!";
            return {};
          }

          std::size_t selected_path_index {};
          const auto &device_data { topology_data_it->second };

          const auto used_source_id { get_already_used_source_id_in_group(device_data.source_adapter_id) };
          if (used_source_id) {
            // Some device in the group is already using the source id and we belong to the same adapter.
            // This means we must also use the path with the same source id.
            auto path_source_it { device_data.source_id_to_path_index.find(*used_source_id) };
            if (path_source_it == std::end(device_data.source_id_to_path_index)) {
              BOOST_LOG(error) << "device " << device_id << " does not have a path with a source id " << *used_source_id << "!";
              return {};
            }

            selected_path_index = path_source_it->second;
          }
          else {
            // Here we want to select a path index that has the lowest index (the "best" of paths), but only
            // if the source id is still free. Technically we don't need to find the lowest index, but that's
            // what will match the Windows' behaviour the closest if we need to create new topology in the end.
            boost::optional<std::size_t> path_index_candidate;
            UINT32 used_source_id {};
            for (const auto [source_id, index] : device_data.source_id_to_path_index) {
              if (is_source_id_already_used(device_data.source_adapter_id, source_id)) {
                continue;
              }

              if (!path_index_candidate || index < *path_index_candidate) {
                path_index_candidate = index;
                used_source_id = source_id;
              }
            }

            if (!path_index_candidate) {
              // Apparently nvidia GPU can only render 4 different sources at a time (according to Google).
              // However, it seems to be true only for physical connections as we also have virtual displays.
              //
              // Virtual displays have different adapter ids than the physical connection ones, but GPU still
              // has to render them, so I dunno how this 4 source limitation makes sense then?
              //
              // In short, this error should not affect virtual displays when the GPU is at its limit.
              BOOST_LOG(error) << "device " << device_id << " cannot be enabled as the adapter has no more free source id (GPU limitation)!";
              return {};
            }

            selected_path_index = *path_index_candidate;
            used_source_adapter_ids_per_adapter[to_string(device_data.source_adapter_id)].insert(used_source_id);
            used_source_adapter_ids_per_adapter_for_group[to_string(device_data.source_adapter_id)] = used_source_id;
          }

          auto selected_path { paths.at(selected_path_index) };

          // All the indexes must be cleared and only the group id specified
          w_utils::set_source_index(selected_path, boost::none);
          w_utils::set_target_index(selected_path, boost::none);
          w_utils::set_desktop_index(selected_path, boost::none);
          w_utils::set_clone_group_id(selected_path, group_id);
          w_utils::set_active(selected_path);  // We also need to mark it as active...

          new_paths.push_back(selected_path);
        }

        group_id++;
      }

      return new_paths;
    }

    /*!
     * Try to set to the new topology.
     *
     * Either by trying to reuse preexisting ones or creating a new
     * topology that Windows has never seen before.
     *
     * In both cases we are not handling mode information - we are asking Windows to
     * select the previously known modes from DB or create the "best" modes for a
     * new topology.
     */
    bool
    do_set_topology(const active_topology_t &new_topology) {
      auto display_data { w_utils::query_display_config(w_utils::ALL_DEVICES) };
      if (!display_data) {
        // Error already logged
        return false;
      }

      const auto topology_data { make_device_topology_data(display_data->paths) };
      if (topology_data.empty()) {
        // Error already logged
        return false;
      }

      auto paths { make_new_paths_for_topology(new_topology, topology_data, display_data->paths) };
      if (paths.empty()) {
        // Error already logged
        return false;
      }

      UINT32 flags { SDC_APPLY | SDC_TOPOLOGY_SUPPLIED | SDC_ALLOW_PATH_ORDER_CHANGES | SDC_VIRTUAL_MODE_AWARE };
      LONG result { SetDisplayConfig(paths.size(), paths.data(), 0, nullptr, flags) };
      if (result != ERROR_SUCCESS) {
        BOOST_LOG(warning) << w_utils::get_ccd_error_string(result) << " failed to change topology using the topology from Windows DB! Trying to make Windows create the topology.";

        flags = SDC_VALIDATE | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES /* This flag is probably not needed, but who knows really... (not MSDOCS at least) */ | SDC_VIRTUAL_MODE_AWARE | SDC_SAVE_TO_DATABASE;
        result = SetDisplayConfig(paths.size(), paths.data(), 0, nullptr, flags);
        if (result != ERROR_SUCCESS) {
          BOOST_LOG(error) << w_utils::get_ccd_error_string(result) << " failed to create new topology configuration!";
          return false;
        }
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
    const auto topology_data { make_device_topology_data(display_data->paths) };
    if (topology_data.empty()) {
      // Error already logged
      return {};
    }

    for (const auto &[device_id, data] : topology_data) {
      const auto &path { display_data->paths.at(data.get_best_path_index()) };

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
          std::string {},  // Inactive devices can have multiple display names, so it's just meaningless
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
