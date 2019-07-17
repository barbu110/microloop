//
// Copyright (c) 2019 by Victor Barbu. All Rights Reserved.
//

#pragma once

#include <cstdint>
#include <errno.h>
#include <event_source.h>
#include <kernel_exception.h>
#include <limits>
#include <map>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <utils/thread_pool.h>

namespace microloop {

class EventLoop {
  EventLoop() : thread_pool{4}
  {
    epollfd = epoll_create(1);
    if (epollfd == -1) {
      throw KernelException(errno);
    }
  }

public:
  static EventLoop *get_main()
  {
    if (main_instance == nullptr) {
      main_instance = new EventLoop;
    }

    return main_instance;
  }

  void add_event_source(EventSource *event_source)
  {
    std::uint32_t fd = event_source->get_fd();

    auto produced_events = event_source->produced_events();
    if (produced_events) {
      epoll_event ev{};
      ev.events = produced_events;
      ev.data.ptr = static_cast<void *>(event_source);

      if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        throw microloop::KernelException(errno);
      }
    }

    event_sources[fd] = event_source;

    auto start_wrapper = [&event_source, &produced_events]() {
      event_source->start();

      if (!produced_events) {
        /*
         * If the event source does not actually produce any events that may be in the interest of
         * our event loop, then we wait for the "start" function to end and then execute the
         * callback.
         */
        event_source->run_callback();
      }
    };

    if (event_source->native_async()) {
      start_wrapper();
    } else {
      thread_pool.submit(start_wrapper);
    }
  }

  // bool register_signal_handler(SignalsMonitor::SignalHandler callback)  // TODO
  // {
  //   return false;
  // }

  bool next_tick()
  {
    epoll_event events_list[128];

    auto ready = epoll_wait(epollfd, events_list, 128, -1);
    if (ready < 0) {
      return false;
    }

    for (int i = 0; i < ready; i++) {
      auto &event = events_list[i];

      auto event_source = reinterpret_cast<EventSource *>(event.data.ptr);

      if (!event_source->is_complete()) {
        continue;
      }

      if (event_source->needs_retry()) {
        thread_pool.submit(&EventSource::start, event_source);
      } else {
        event_source->run_callback();
        remove_event_source(event_source);
      }
    }

    return true;
  }

  ~EventLoop()
  {
    close(epollfd);
  }

private:
  void remove_event_source(EventSource *event_source)
  {
    if (epoll_ctl(epollfd, EPOLL_CTL_DEL, event_source->get_fd(), nullptr) == -1) {
      throw KernelException(errno);
    }

    event_sources.erase(event_source->get_fd());
    delete event_source;
  }

private:
  int epollfd;
  utils::ThreadPool thread_pool;
  std::map<std::uint64_t, EventSource *> event_sources;

  static EventLoop *main_instance;
};

}  // namespace microloop

#define MICROLOOP_TICK() microloop::EventLoop::get_main()->next_tick()
