#pragma once

// 同一个 DWDNative.exe 的隐藏转码模式入口。
// Controller 通过 --proxy 参数启动它，使昂贵的编码器在独立进程中运行；
// 任务结束后进程退出，编码缓存和工作集都会被操作系统彻底回收。
int RunProxyTranscoder();
