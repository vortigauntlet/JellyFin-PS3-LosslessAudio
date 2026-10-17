// bell::WrappedSemaphore over an lv2 semaphore (bell's own uses POSIX semaphores, which this toolchain lacks).
#pragma once
#include <stdlib.h>
#include <sys/sem.h>

namespace bell {

class WrappedSemaphore {
 public:
  WrappedSemaphore(int maxVal = 200) {
    sys_sem_attr_t attr;
    attr.attr_protocol = SYS_SEM_ATTR_PROTOCOL;
    attr.attr_pshared = SYS_SEM_ATTR_PSHARED;
    attr.key = 0;
    attr.flags = 0;
    attr.pad = 0;
    attr.name[0] = '\0';
    if (sysSemCreate(&sem_, &attr, 0, maxVal < 1 ? 200 : maxVal) != 0) abort();
  }
  ~WrappedSemaphore() { sysSemDestroy(sem_); }
  WrappedSemaphore(const WrappedSemaphore&) = delete;
  WrappedSemaphore& operator=(const WrappedSemaphore&) = delete;

  int wait() { sysSemWait(sem_, 0); return 0; }
  // 0 when the semaphore was taken, -1 on timeout (sem_timedwait's contract)
  int twait(long milliseconds = 10) {
    const u64 us = milliseconds < 1 ? 1 : (u64)milliseconds * 1000ULL;
    return sysSemWait(sem_, us) == 0 ? 0 : -1;
  }
  void give() { sysSemPost(sem_, 1); }

 private:
  sys_sem_t sem_;
};

}  // namespace bell
