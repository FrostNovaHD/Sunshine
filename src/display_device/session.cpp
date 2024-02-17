// standard includes
#include <thread>

// local includes
#include "session.h"
#include "src/platform/common.h"
#include "to_string.h"

namespace display_device {

  class session_t::StateRestoreRetryTimer {
  public:
    StateRestoreRetryTimer(std::mutex &mutex, settings_t &settings, std::chrono::milliseconds timeout_duration):
        mutex { mutex }, settings { settings }, timeout_duration { timeout_duration }, timer_thread {
          std::thread { [this]() {
            std::unique_lock<std::mutex> lock { this->mutex };
            while (keep_alive) {
              can_wake_up = false;
              if (next_wake_up_time) {
                sleep_cv.wait_until(lock, *next_wake_up_time, [this]() { return can_wake_up; });
              }
              else {
                sleep_cv.wait(lock, [this]() { return can_wake_up; });
              }

              if (next_wake_up_time) {
                // Timer has been started freshly or we have waited for the required amount of time.
                // We can check this by comparing time points.
                const auto now { std::chrono::steady_clock::now() };
                if (now < *next_wake_up_time) {
                  // Thread has been waken up by `start_unlocked` to synchronize the time point.
                  // We do nothing and just go back to waiting with a new time point.
                }
                else {
                  const auto result { this->settings.revert_settings() };
                  if (result) {
                    next_wake_up_time = boost::none;
                  }
                  else {
                    next_wake_up_time = now + this->timeout_duration;
                  }
                }
              }
              else {
                // Timer has been stopped.
                // We do nothing and just go back to waiting until notified (unless we are killing the thread).
              }
            }
          } }
        } {
    }

    ~StateRestoreRetryTimer() {
      {
        std::lock_guard lock { mutex };
        keep_alive = false;
        next_wake_up_time = boost::none;
        wake_up_thread();
      }

      timer_thread.join();
    }

    void
    setup_timer_unlocked(bool start) {
      if (start) {
        next_wake_up_time = std::chrono::steady_clock::now() + timeout_duration;
      }
      else {
        if (!next_wake_up_time) {
          return;
        }

        next_wake_up_time = boost::none;
      }

      wake_up_thread();
    }

  private:
    void
    wake_up_thread() {
      can_wake_up = true;
      sleep_cv.notify_one();
    }

    std::mutex &mutex;
    settings_t &settings;
    std::chrono::milliseconds timeout_duration;

    std::thread timer_thread;
    std::condition_variable sleep_cv;

    bool can_wake_up { false };
    bool keep_alive { true };
    boost::optional<std::chrono::steady_clock::time_point> next_wake_up_time;
  };

  session_t::deinit_t::~deinit_t() {
    session_t::get().restore_state();
  }

  session_t &
  session_t::get() {
    static session_t session;
    return session;
  }

  std::unique_ptr<session_t::deinit_t>
  session_t::init() {
    const auto devices { enum_available_devices() };
    if (!devices.empty()) {
      BOOST_LOG(info) << "available display devices: " << to_string(devices);
    }

    session_t::get().settings.set_filepath(platf::appdata() / "original_display_settings.json");
    session_t::get().restore_state();
    return std::make_unique<deinit_t>();
  }

  settings_t::apply_result_t
  session_t::configure_display(const config::video_t &config, const rtsp_stream::launch_session_t &session) {
    std::lock_guard lock { mutex };

    const auto result { settings.apply_config(config, session) };
    timer->setup_timer_unlocked(!result);
    return result;
  }

  void
  session_t::restore_state() {
    std::lock_guard lock { mutex };

    const auto result { settings.revert_settings() };
    timer->setup_timer_unlocked(!result);
  }

  void
  session_t::reset_persistence() {
    std::lock_guard lock { mutex };

    settings.reset_persistence();
    timer->setup_timer_unlocked(false);
  }

  session_t::session_t():
      timer { std::make_unique<StateRestoreRetryTimer>(mutex, settings, std::chrono::milliseconds { 30 * 1000 }) } {
  }

}  // namespace display_device
