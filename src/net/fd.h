// RAII管理fd

#pragma once
#include <unistd.h>

namespace mc {
    class Fd {
    public:
        Fd() = default;
        explicit Fd(int fd) : fd_(fd) {}
        ~Fd() { reset(); }

        Fd(const Fd&) = delete;     // fd不能被复制，只能转移所有权
        Fd& operator=(const Fd&) = delete;
        Fd(Fd&& o) noexcept : fd_(o.release()) {}
        Fd& operator=(Fd&& o) noexcept {
            if (this != &o)
                reset(o.release());
            return *this;
        }

        int get() const { return fd_; }
        explicit operator bool() const { return fd_ >= 0; }

        int release() {
            int t = fd_;
            fd_ = -1;
            return t;
        }
        void reset(int fd = -1) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = fd;
        }

    private:
        int fd_ = -1;
    };
}