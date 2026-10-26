#include "./event_loop.h"

#include <cerrno>

namespace mc {
    EventLoop::EventLoop() : epfd_(::epoll_create(EPOLL_CLOEXEC)) {}

    bool EventLoop::add(int fd, uint32_t events, Handler h) {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        if (::epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, fd, &ev) != 0)
            return false;
        handlers_[fd] = std::make_shared<Handler>(std::move(h));
        return true;
    }

    bool EventLoop::mod(int fd, uint32_t events) {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        return ::epoll_ctl(epfd_.get(), EPOLL_CTL_MOD, fd, &ev) == 0;
    }

    void EventLoop::del(int fd) {
        ::epoll_ctl(epfd_.get(), EPOLL_CTL_DEL, fd, nullptr);
        handlers_.erase(fd);
    }

    int EventLoop::poll(int timeout_ms) {
        epoll_event evs[128];
        int n = ::epoll_wait(epfd_.get(), evs, 128, timeout_ms);
        if (n < 0)
            return errno == EINTR ? 0 : -1;
        for (int i = 0; i < n; ++i) {
            // 每次分发都重新查表，前一个回调已经关闭了这个fd
            auto it = handlers_.find(evs[i].data.fd);
            if (it == handlers_.end())
                continue;
            // 拷一份shared_ptr：回调执行期间可能del自己，不能让正在执行的std::function被销毁
            std::shared_ptr<Handler> h = it->second;
            (*h)(evs[i].events);
        }
        return n;
    }
}