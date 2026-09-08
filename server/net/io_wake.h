#pragma once
// -----------------------------------------------------------------------------
//  server/net/io_wake.h  —  epoll IO 线程唤醒机制（eventfd）
//  归属 L1。
//  业务线程入队后调 wakeIoLoop() 写一字节唤醒 epoll_wait，由 IO 线程重新 arm EPOLLOUT。
// -----------------------------------------------------------------------------
namespace ecp {

bool initIoWakeup();   // 创建 eventfd，IO 线程启动前调用一次
void wakeIoLoop();     // 任意线程：写一字节唤醒 epoll_wait（非阻塞）
int  ioWakeupFd();     // 取 eventfd 供 epoll 注册；未初始化返回 -1

} // namespace ecp
