#pragma once
#include <sys/epoll.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

#include "./fd.h"

namespace mc {
    // 极简Reactor：fd就绪时调用登记的回调，只能在一个线程里使用
    class EventLoop {
    public:
        using Handler = std::function<void(uint32_t events)>;

        EventLoop();
        bool valid() const { return static_cast<bool>(epfd_); }

        bool add(int fd, uint32_t events, Handler h);
        bool mod(int fd, uint32_t events);
        void del(int fd);               // 必须在close(fd)之前调用

        // 等待并分发一轮事件，返回事件数，出错返回-1
        int poll(int timeout_ms);

    private:
        Fd epfd_;
        std::unordered_map<int, std::shared_ptr<Handler>> handlers_;
    };
}

