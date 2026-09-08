#include "io_wake.h"

#include <cstdint>
#include <sys/eventfd.h>
#include <unistd.h>

namespace ecp {

static int g_wakeFd = -1;

bool initIoWakeup()
{
    g_wakeFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    return g_wakeFd >= 0;
}

int ioWakeupFd() { return g_wakeFd; }

void wakeIoLoop()
{
    if (g_wakeFd < 0) return;
    const std::uint64_t one = 1;
    const ssize_t ignored = ::write(g_wakeFd, &one, sizeof(one));   // EFD_NONBLOCK：满了只丢唤醒，不阻塞
    (void)ignored;
}

} // namespace ecp
