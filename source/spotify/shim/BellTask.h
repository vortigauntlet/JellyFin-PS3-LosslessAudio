// bell::Task over a lv2 PPU thread (bell's own uses pthreads, which this toolchain does not implement).
// The task object must outlive its thread; cspot guards that with a mutex held for the whole of runTask().
#pragma once
#include <string>
#include <sys/thread.h>

namespace bell {

class Task {
 public:
  std::string TASK;
  int stackSize, core;
  bool runOnPSRAM;
  Task(std::string taskName, int stackSize, int priority, int core, bool runOnPSRAM = true)
      : TASK(taskName), stackSize(stackSize), core(core), runOnPSRAM(runOnPSRAM), prio_(priority) {}
  virtual ~Task() {}

  bool startTask() {
    // Thread stacks come out of the same heap as everything else, so the sizes bell asks for (4-48 KB on a microcontroller)
    // are raised to what C++ code with std::string and nanopb needs; a 256 KB stack failed in the music code, 128 KB is the floor.
    u64 stack = (u64)stackSize;
    if (stack < 128 * 1024) stack = 128 * 1024;
    char name[24];
    snprintf(name, sizeof name, "%.20s", TASK.c_str());
    sys_ppu_thread_t tid;
    return sysThreadCreate(&tid, entry, this, 1001 + prio_, stack, 0, name) == 0;   // 0: detached
  }

 protected:
  virtual void runTask() = 0;

 private:
  static void entry(void* self) {
    static_cast<Task*>(self)->runTask();
    sysThreadExit(0);
  }
  int prio_;
};

}  // namespace bell
