// standard includes
#include <fstream>
#include <thread>

// local includes
#include "settings_topology.h"
#include "src/audio.h"
#include "src/display_device/to_string.h"
#include "src/logging.h"

namespace display_device {

  namespace {

    bool
    contains_modifications(const settings_t::persistent_data_t &data) {
      return !is_topology_the_same(data.topology.initial, data.topology.modified) ||
             !data.original_primary_display.empty() ||
             !data.original_modes.empty() ||
             !data.original_hdr_states.empty();
    }

    template <class T>
    const T &
    get_original_value(const T &current_value, const T &previous_value) {
      return previous_value.empty() ? current_value : previous_value;
    }

    std::string
    get_current_primary_display(const topology_metadata_t &metadata) {
      for (const auto &group : metadata.current_topology) {
        for (const auto &device_id : group) {
          if (is_primary_device(device_id)) {
            return device_id;
          }
        }
      }

      return std::string {};
    }

    std::string
    determine_new_primary_display(const std::string &original_primary_display, const topology_metadata_t &metadata) {
      if (metadata.primary_device_requested) {
        // Primary device was requested - no device was specified by user.
        // This means we are keeping the original primary display.
        return original_primary_display;
      }

      // For primary devices it is enough to set 1 as a primary as the whole duplicated group
      // will become primary devices.
      const auto new_primary_device { metadata.duplicated_devices.front() };
      return new_primary_device;
    }

    boost::optional<std::string>
    handle_primary_display_configuration(const parsed_config_t::device_prep_e &device_prep, const std::string &previous_primary_display, const topology_metadata_t &metadata) {
      if (device_prep == parsed_config_t::device_prep_e::ensure_primary) {
        const auto original_primary_display { get_original_value(get_current_primary_display(metadata), previous_primary_display) };
        const auto new_primary_display { determine_new_primary_display(original_primary_display, metadata) };

        BOOST_LOG(debug) << "changing primary display to: " << new_primary_display;
        if (!set_as_primary_device(new_primary_display)) {
          // Error already logged
          return boost::none;
        }

        return original_primary_display;
      }

      if (!previous_primary_display.empty()) {
        BOOST_LOG(debug) << "changing primary display back to: " << previous_primary_display;
        if (!set_as_primary_device(previous_primary_display)) {
          // Error already logged
          return boost::none;
        }
      }

      return std::string {};
    }

    device_display_mode_map_t
    determine_new_display_modes(const boost::optional<resolution_t> &resolution, const boost::optional<refresh_rate_t> &refresh_rate, const device_display_mode_map_t &original_display_modes, const topology_metadata_t &metadata) {
      device_display_mode_map_t new_modes { original_display_modes };

      if (resolution) {
        // For duplicate devices the resolution must match no matter what
        for (const auto &device_id : metadata.duplicated_devices) {
          new_modes[device_id].resolution = *resolution;
        }
      }

      if (refresh_rate) {
        if (metadata.primary_device_requested) {
          // No device has been specified, so if they're all are primary devices
          // we need to apply the refresh rate change to all duplicates
          for (const auto &device_id : metadata.duplicated_devices) {
            new_modes[device_id].refresh_rate = *refresh_rate;
          }
        }
        else {
          // Even if we have duplicate devices, their refresh rate may differ
          // and since the device was specified, let's apply the refresh
          // rate only to the specified device.
          new_modes[metadata.duplicated_devices.front()].refresh_rate = *refresh_rate;
        }
      }

      return new_modes;
    }

    boost::optional<device_display_mode_map_t>
    handle_display_mode_configuration(const boost::optional<resolution_t> &resolution, const boost::optional<refresh_rate_t> &refresh_rate, const device_display_mode_map_t &previous_display_modes, const topology_metadata_t &metadata) {
      if (resolution || refresh_rate) {
        const auto original_display_modes { get_original_value(get_current_display_modes(get_device_ids_from_topology(metadata.current_topology)), previous_display_modes) };
        const auto new_display_modes { determine_new_display_modes(resolution, refresh_rate, original_display_modes, metadata) };

        BOOST_LOG(debug) << "changing display modes to: " << to_string(new_display_modes);
        if (!set_display_modes(new_display_modes)) {
          // Error already logged
          return boost::none;
        }

        return original_display_modes;
      }

      if (!previous_display_modes.empty()) {
        BOOST_LOG(debug) << "changing display modes back to: " << to_string(previous_display_modes);
        if (!set_display_modes(previous_display_modes)) {
          // Error already logged
          return boost::none;
        }
      }

      return device_display_mode_map_t {};
    }

    /*!
     * Some newly enabled displays do not handle HDR state correctly (IDD HDR display for example).
     * The colors can become very blown out/high contrast. A simple workaround is to toggle the HDR state
     * once the display has "settled down" or something.
     *
     * This is what this function does, it changes the HDR state to the opposite once that we will have in the
     * end, sleeps for a little and then allows us to continue changing HDR states to the final ones.
     *
     * "blank" comes as an inspiration from "vblank" as this function is meant to be used before changing the HDR
     * states to clean up something.
     */
    bool
    blank_hdr_states(const hdr_state_map_t &states, const std::unordered_set<std::string> &newly_enabled_devices) {
      const std::chrono::milliseconds delay { 1500 };
      if (delay > std::chrono::milliseconds::zero()) {
        bool state_changed { false };
        auto toggled_states { states };
        for (const auto &device_id : newly_enabled_devices) {
          auto state_it { toggled_states.find(device_id) };
          if (state_it == std::end(toggled_states)) {
            continue;
          }

          if (state_it->second == hdr_state_e::enabled) {
            state_it->second = hdr_state_e::disabled;
            state_changed = true;
          }
          else if (state_it->second == hdr_state_e::disabled) {
            state_it->second = hdr_state_e::enabled;
            state_changed = true;
          }
        }

        if (state_changed) {
          BOOST_LOG(debug) << "toggling HDR states for newly enabled devices and waiting for " << delay.count() << "ms before actually applying the correct states.";
          if (!set_hdr_states(toggled_states)) {
            // Error already logged
            return false;
          }

          std::this_thread::sleep_for(delay);
        }
      }

      return true;
    }

    hdr_state_map_t
    determine_new_hdr_states(const boost::optional<bool> &change_hdr_state, const hdr_state_map_t &original_hdr_states, const topology_metadata_t &metadata) {
      hdr_state_map_t new_states { original_hdr_states };

      if (change_hdr_state) {
        const hdr_state_e end_state { *change_hdr_state ? hdr_state_e::enabled : hdr_state_e::disabled };
        const auto try_update_new_state = [&new_states, end_state](const std::string &device_id) {
          const auto current_state { new_states[device_id] };
          if (current_state == hdr_state_e::unknown) {
            return;
          }

          new_states[device_id] = end_state;
        };

        if (metadata.primary_device_requested) {
          // No device has been specified, so if they're all are primary devices
          // we need to apply the HDR state change to all duplicates
          for (const auto &device_id : metadata.duplicated_devices) {
            try_update_new_state(device_id);
          }
        }
        else {
          // Even if we have duplicate devices, their HDR states may differ
          // and since the device was specified, let's apply the HDR state
          // only to the specified device.
          try_update_new_state(metadata.duplicated_devices.front());
        }
      }

      return new_states;
    }

    boost::optional<hdr_state_map_t>
    handle_hdr_state_configuration(const boost::optional<bool> &change_hdr_state, const hdr_state_map_t &previous_hdr_states, const topology_metadata_t &metadata) {
      if (change_hdr_state) {
        const auto original_hdr_states { get_original_value(get_current_hdr_states(get_device_ids_from_topology(metadata.current_topology)), previous_hdr_states) };
        const auto new_hdr_states { determine_new_hdr_states(change_hdr_state, original_hdr_states, metadata) };

        BOOST_LOG(debug) << "changing hdr states to: " << to_string(new_hdr_states);
        if (!blank_hdr_states(new_hdr_states, metadata.newly_enabled_devices) || !set_hdr_states(new_hdr_states)) {
          // Error already logged
          return boost::none;
        }

        return original_hdr_states;
      }

      if (!previous_hdr_states.empty()) {
        BOOST_LOG(debug) << "changing hdr states back to: " << to_string(previous_hdr_states);
        if (!blank_hdr_states(previous_hdr_states, metadata.newly_enabled_devices) || !set_hdr_states(previous_hdr_states)) {
          // Error already logged
          return boost::none;
        }
      }

      return hdr_state_map_t {};
    }

    bool
    try_revert_settings(settings_t::persistent_data_t &data, bool &data_modified) {
      // On Windows settings are saved per an active topology list/pairing/set.
      // This makes it complicated when having to revert the changes as we MUST
      // be in the same topology we made those changes to (except for HDR, because it's
      // not a part of a path/mode lists that are used for topology, but the display
      // still needs to be activate to change it).
      //
      // Unplugging inactive devices does not change the topology, however plugging one
      // in will (maybe), as Windows seems to try to activate the device automatically. Unplugging
      // active device will also change the topology.

      if (!contains_modifications(data)) {
        return true;
      }

      const bool have_changes_for_modified_topology { !data.original_primary_display.empty() || !data.original_modes.empty() || !data.original_hdr_states.empty() };
      std::unordered_set<std::string> newly_enabled_devices;
      bool partially_failed { false };
      auto current_topology { get_current_topology() };

      if (have_changes_for_modified_topology) {
        if (set_topology(data.topology.modified)) {
          newly_enabled_devices = get_newly_enabled_devices_from_topology(current_topology, data.topology.modified);
          current_topology = data.topology.modified;

          if (!data.original_hdr_states.empty()) {
            BOOST_LOG(debug) << "changing back the HDR states to: " << to_string(data.original_hdr_states);
            if (set_hdr_states(data.original_hdr_states)) {
              data.original_hdr_states.clear();
            }
            else {
              partially_failed = true;
            }
          }

          if (!data.original_modes.empty()) {
            BOOST_LOG(debug) << "changing back the display modes to: " << to_string(data.original_modes);
            if (set_display_modes(data.original_modes)) {
              data.original_modes.clear();
            }
            else {
              partially_failed = true;
            }
          }

          if (!data.original_modes.empty()) {
            BOOST_LOG(debug) << "changing back the display modes to: " << to_string(data.original_modes);
            if (set_display_modes(data.original_modes)) {
              data.original_modes.clear();
            }
            else {
              partially_failed = true;
            }
          }

          if (!data.original_primary_display.empty()) {
            BOOST_LOG(debug) << "changing back the primary device to: " << data.original_primary_display;
            if (set_as_primary_device(data.original_primary_display)) {
              data.original_primary_display.clear();
            }
            else {
              partially_failed = true;
            }
          }
        }
        else {
          BOOST_LOG(warning) << "cannot switch to the topology to undo changes!";
          partially_failed = true;
        }
      }

      if (set_topology(data.topology.initial)) {
        newly_enabled_devices.merge(get_newly_enabled_devices_from_topology(current_topology, data.topology.initial));
        current_topology = data.topology.initial;
      }
      else {
        BOOST_LOG(warning) << "failed to switch back to the initial topology!";
        partially_failed = true;
      }

      if (!newly_enabled_devices.empty()) {
        const auto current_hdr_states { get_current_hdr_states(get_device_ids_from_topology(current_topology)) };

        BOOST_LOG(debug) << "trying to fix HDR states (if needed).";
        blank_hdr_states(current_hdr_states, newly_enabled_devices);  // Return value ignored
        set_hdr_states(current_hdr_states);  // Return value ignored
      }

      return !partially_failed;
    }

    bool
    save_settings(const std::filesystem::path &filepath, const settings_t::persistent_data_t &data) {
      if (!filepath.empty()) {
        try {
          std::ofstream file(filepath, std::ios::out | std::ios::trunc);
          nlohmann::json json_data = data;

          // Write json with indentation
          file << std::setw(4) << json_data << std::endl;
          return true;
        }
        catch (const std::exception &err) {
          BOOST_LOG(info) << "Failed to save display settings: " << err.what();
        }
      }

      return false;
    }

    std::unique_ptr<settings_t::persistent_data_t>
    load_settings(const std::filesystem::path &filepath) {
      try {
        if (!filepath.empty() && std::filesystem::exists(filepath)) {
          std::ifstream file(filepath);
          return std::make_unique<settings_t::persistent_data_t>(nlohmann::json::parse(file));
        }
      }
      catch (const std::exception &err) {
        BOOST_LOG(info) << "Failed to load saved display settings: " << err.what();
      }

      return nullptr;
    }

    void
    remove_file(const std::filesystem::path &filepath) {
      try {
        if (!filepath.empty()) {
          std::filesystem::remove(filepath);
        }
      }
      catch (const std::exception &err) {
        BOOST_LOG(error) << "failed to remove " << filepath << ". Error: " << err.what();
      }
    }

  }  // namespace

  //! A simple struct that automatically captures the audio ctx reference.
  struct settings_t::audio_data_t {
    decltype(audio::get_audio_ctx_ref()) audio_ctx_ref { audio::get_audio_ctx_ref() };
  };

  settings_t::settings_t() {
  }

  settings_t::~settings_t() {
  }

  settings_t::apply_result_t
  settings_t::apply_config(const config::video_t &config, const rtsp_stream::launch_session_t &session) {
    BOOST_LOG(info) << "Applying configuration to the display device.";
    const auto parsed_config { make_parsed_config(config, session) };
    if (!parsed_config) {
      BOOST_LOG(error) << "Failed to apply configuration to the display device.";
      return { apply_result_t::result_e::config_parse_fail };
    }

    const bool display_may_change { parsed_config->device_prep == parsed_config_t::device_prep_e::ensure_only_display };
    if (display_may_change && !audio_data) {
      // It is very likely that in this situation our "current" audio device will be gone, so we
      // want to capture the audio sink immediately and extend the session until we revert our changes
      BOOST_LOG(debug) << "Capturing audio sink before changing display";
      audio_data = std::make_unique<audio_data_t>();
    }

    const auto result { apply_config(*parsed_config) };
    if (result) {
      if (!display_may_change && audio_data) {
        // Just to be safe in the future when the video config can be reloaded
        // without Sunshine restarting, we should cleanup
        BOOST_LOG(debug) << "Releasing captured audio sink";
        audio_data = nullptr;
      }
    }

    BOOST_LOG(info) << "Display device configuration applied.";
    return result;
  }

  bool
  settings_t::revert_settings() {
    if (!persistent_data) {
      BOOST_LOG(info) << "Loading persistent display device settings.";
      persistent_data = load_settings(filepath);
    }

    if (persistent_data) {
      BOOST_LOG(info) << "Reverting display device settings.";

      bool data_updated { false };
      if (!try_revert_settings(*persistent_data, data_updated)) {
        if (data_updated) {
          save_settings(filepath, *persistent_data);  // Ignoring return value
        }

        BOOST_LOG(error) << "Failed to revert display device settings!";
        return false;
      }

      remove_file(filepath);
      persistent_data = nullptr;

      if (audio_data) {
        BOOST_LOG(debug) << "Releasing captured audio sink";
        audio_data = nullptr;
      }

      BOOST_LOG(info) << "Display device configuration reset.";
    }

    return true;
  }

  void
  settings_t::reset_persistence() {
    BOOST_LOG(info) << "Purging persistent display device data (trying to reset settings one last time).";
    if (persistent_data && !revert_settings()) {
      BOOST_LOG(info) << "Failed to revert settings - proceeding to reset persistence.";
    }

    remove_file(filepath);
    persistent_data = nullptr;
    audio_data = nullptr;
  }

  settings_t::apply_result_t
  settings_t::apply_config(const parsed_config_t &config) {
    // The idea behind this method is simple.
    //
    // We take a original settings as our base. The original settings can be either the
    // settings from when we applied configuration for the first time, or if we don't have
    // a original settings from previous configuration, we take the current settings.
    //
    // We then apply new settings over our base settings. By doing this we make sure
    // that we always have a clean slate - if we apply config multiple times, the settings
    // will not accumulate and the things that we don't configure will be automatically
    // reverted.

    bool failed_while_reverting { false };
    const boost::optional<topology_data_t> previously_configured_topology { persistent_data ? boost::make_optional(persistent_data->topology) : boost::none };
    const auto topology_result { handle_device_topology_configuration(config, previously_configured_topology, [&]() {
      const bool audio_sink_was_captured { audio_data != nullptr };
      if (!revert_settings()) {
        failed_while_reverting = true;
        return false;
      }

      if (audio_sink_was_captured && !audio_data) {
        audio_data = std::make_unique<audio_data_t>();
      }
      return true;
    }) };
    if (!topology_result) {
      // Error already logged
      return { failed_while_reverting ? apply_result_t::result_e::revert_fail : apply_result_t::result_e::topology_fail };
    }

    persistent_data_t new_settings { topology_result->topology_data };
    persistent_data_t &current_settings { persistent_data ? *persistent_data : new_settings };

    const auto persist_settings = [&]() -> apply_result_t {
      if (contains_modifications(current_settings)) {
        if (!persistent_data) {
          persistent_data = std::make_unique<persistent_data_t>(new_settings);
        }

        if (!save_settings(filepath, *persistent_data)) {
          return { apply_result_t::result_e::file_save_fail };
        }
      }
      else if (persistent_data) {
        if (!revert_settings()) {
          // Sanity
          return { apply_result_t::result_e::revert_fail };
        }
      }

      return { apply_result_t::result_e::success };
    };
    auto save_guard = util::fail_guard([&]() {
      persist_settings();  // Ignoring the return value
    });

    const auto original_primary_display { handle_primary_display_configuration(config.device_prep, current_settings.original_primary_display, topology_result->metadata) };
    if (!original_primary_display) {
      // Error already logged
      return { apply_result_t::result_e::primary_display_fail };
    }
    current_settings.original_primary_display = *original_primary_display;

    const auto original_modes { handle_display_mode_configuration(config.resolution, config.refresh_rate, current_settings.original_modes, topology_result->metadata) };
    if (!original_modes) {
      // Error already logged
      return { apply_result_t::result_e::modes_fail };
    }
    current_settings.original_modes = *original_modes;

    const auto original_hdr_states { handle_hdr_state_configuration(config.change_hdr_state, current_settings.original_hdr_states, topology_result->metadata) };
    if (!original_hdr_states) {
      // Error already logged
      return { apply_result_t::result_e::hdr_states_fail };
    }
    current_settings.original_hdr_states = *original_hdr_states;

    save_guard.disable();
    return persist_settings();
  }

}  // namespace display_device
