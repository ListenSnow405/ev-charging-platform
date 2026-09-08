#pragma once
// -----------------------------------------------------------------------------
//  server/net/io_wake.h  —  epoll IO 线程唤醒（eventfd）
//  归属 L1。
//  任意线程可调用 wakeIoLoop() 写一字节唤醒 epoll_wait，由 IO 线程重新 arm EPOLLOUT。
//
//  使用说明：内部唤醒机制，禁止外部操作原始 wake fd；不要多次 init、不要外部 close。
//
//  依赖：glibc GNU 扩展 (eventfd)
//  线程：全部接口线程安全；仅内部初始化 / 销毁在 IO 线程
// -----------------------------------------------------------------------------
namespace ecp {

bool initIoWakeup();   // 创建 eventfd，IO 线程启动前调用一次
void wakeIoLoop();     // 任意线程：写一字节唤醒 epoll_wait（非阻塞）
int  ioWakeupFd();     // 取 eventfd 供 epoll 注册；未初始化返回 -1

} // namespace ecp
