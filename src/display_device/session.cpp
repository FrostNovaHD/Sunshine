// standard includes
#include <thread>

// local includes
#include "session.h"
#include "src/platform/common.h"
#include "to_string.h"

namespace display_device {

  class session_t::StateRestoreRetryTimer {
  public:
    /**
     * @brief A constructor for the timer.
     * @param mutex A shared mutex for synchronization.
     * @param settings A shared settings instance for reverting settings.
     * @param timeout_duration An amount of time to wait until retrying.
     * @warning Because we are keeping references to shared parameters, we MUST ensure they outlive this object!
     */
    StateRestoreRetryTimer(std::mutex &mutex, settings_t &settings, std::chrono::milliseconds timeout_duration):
        mutex { mutex }, settings { settings }, timeout_duration { timeout_duration }, timer_thread {
          std::thread { [this]() {
            std::unique_lock<std::mutex> lock { this->mutex };
            while (keep_alive) {
              can_wake_up = false;
              if (next_wake_up_time) {
                // We're going to sleep forever until manually woken up or the time elapses
                sleep_cv.wait_until(lock, *next_wake_up_time, [this]() { return can_wake_up; });
              }
              else {
                // We're going to sleep forever until manually woken up
                sleep_cv.wait(lock, [this]() { return can_wake_up; });
              }

              if (next_wake_up_time) {
                // Timer has just been started, or we have waited for the required amount of time.
                // We can check which case it is by comparing time points.

                const auto now { std::chrono::steady_clock::now() };
                if (now < *next_wake_up_time) {
                  // Thread has been woken up manually to synchronize the time points.
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

    /**
     * @brief A destructor for the timer that gracefully shuts down the thread.
     */
    ~StateRestoreRetryTimer() {
      {
        std::lock_guard lock { mutex };
        keep_alive = false;
        next_wake_up_time = boost::none;
        wake_up_thread();
      }

      timer_thread.join();
    }

    /**
     * @brief Start or stop the timer thread.
     * @param start Indicate whether to start or stop the timer.
     *              True - start or restart the timer to be executed after the specified duration from now.
     *              False - stop the timer and put the thread to sleep.
     * @warning This method does NOT acquire the mutex! It is intended to be used from places
     *          where the mutex has already been locked.
     */
    void
    setup_timer(bool start) {
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
    /**
     * @brief Manually wake up the thread.
     */
    void
    wake_up_thread() {
      can_wake_up = true;
      sleep_cv.notify_one();
    }

    std::mutex &mutex; /**< A reference to a shared mutex. */
    settings_t &settings; /**< A reference to a shared settings instance. */
    std::chrono::milliseconds timeout_duration; /**< A retry time for the timer. */

    std::thread timer_thread; /**< A timer thread. */
    std::condition_variable sleep_cv; /**< Condition variable for waking up thread. */

    bool can_wake_up { false }; /**< Safeguard for the condition variable to prevent sporadic thread wake ups. */
    bool keep_alive { true }; /**< A kill switch for the thread when it has been woken up. */
    boost::optional<std::chrono::steady_clock::time_point> next_wake_up_time; /**< Next time point for thread to wake up. */
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
    timer->setup_timer(!result);
    return result;
  }

  void
  session_t::restore_state() {
    std::lock_guard lock { mutex };

    const auto result { settings.revert_settings() };
    timer->setup_timer(!result);
  }

  void
  session_t::reset_persistence() {
    std::lock_guard lock { mutex };

    settings.reset_persistence();
    timer->setup_timer(false);
  }

  session_t::session_t():
      timer { std::make_unique<StateRestoreRetryTimer>(mutex, settings, std::chrono::seconds { 30 }) } {
  }

}  // namespace display_device
