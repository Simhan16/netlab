#pragma once

#include <unistd.h>

#include <utility>

namespace netlab {

// Owns a file descriptor and closes it exactly once. Move-only, so ownership
// is always visible in the type: a function taking `Fd` takes the socket.
class Fd {
 public:
  Fd() = default;
  explicit Fd(int fd) noexcept : fd_(fd) {}
  ~Fd() { reset(); }

  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }

  int get() const noexcept { return fd_; }
  bool valid() const noexcept { return fd_ >= 0; }
  int release() noexcept { return std::exchange(fd_, -1); }
  void reset(int fd = -1) noexcept {
    if (fd_ >= 0) ::close(fd_);
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};

}  // namespace netlab
