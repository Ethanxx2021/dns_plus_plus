#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

// benchmarks/bench_common.h
//
// 两个 benchmark(bench_broker / bench_multi_broker)共用的测量方法学辅助函数:
//   1. 环境元数据(stderr 头部):CPU 型号、核数、内核版本、GMP 运行时版本、编译类型。
//   2. /proc/net/snmp 的 Udp 段 RcvbufErrors / InErrors 读取(用于区分 recall 下降是
//      算法过滤还是内核 UDP 丢包)。
//   3. 订阅者 socket 的 SO_RCVBUF 设置 + getsockopt 回读实际生效值。
//
// 这些函数只做测量与可观测性,不改动 recall / stretch 的计算逻辑。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <iostream>
#include <algorithm>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <gmp.h>

namespace bench {

// 去掉首尾空白(用于解析 /proc/cpuinfo 的 model name)。
inline std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// 把环境元数据打印到 stderr 头部。每一项一行、带固定前缀,便于脚本 grep 进 env.txt。
// git commit hash 这里刻意不取:取它要么在运行时 fork+exec(增加测量路径上的运行时
// 开销),要么在编译期嵌入(需要额外 CMake 机制且换 commit 不重配会过期)。驱动脚本
// scripts/run_full_eval.sh 会在 env.txt 里用 `git rev-parse HEAD` 记录它。
inline void printEnvironment() {
    std::string model = "unknown";
    {
        std::ifstream f("/proc/cpuinfo");
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("model name", 0) == 0) {
                size_t p = line.find(':');
                if (p != std::string::npos) model = trim(line.substr(p + 1));
                break;
            }
        }
    }

    long nproc = sysconf(_SC_NPROCESSORS_ONLN);

    std::string kernel = "unknown";
    struct utsname u;
    if (uname(&u) == 0) kernel = u.release;

    // gmp_version 是 libgmp 提供的运行时版本字符串(而非编译期 __GNU_MP_VERSION)。
    std::string gmp_ver = gmp_version;

    // Release 构建 CMake 会定义 -DNDEBUG;Debug/ASan 不会。用它区分编译类型。
    std::string build_type;
#ifdef NDEBUG
    build_type = "Release";
#else
    build_type = "Debug";
#endif

    std::cerr << "[bench-env] cpu_model=" << model << "\n";
    std::cerr << "[bench-env] nproc=" << nproc << "\n";
    std::cerr << "[bench-env] kernel=" << kernel << "\n";
    std::cerr << "[bench-env] gmp=" << gmp_ver << "\n";
    std::cerr << "[bench-env] build=" << build_type << "\n";
    std::cerr << "[bench-env] git=not-embedded (see run_full_eval.sh env.txt)\n";
}

// /proc/net/snmp 里 Udp 段的一次快照。
struct UdpSnmp {
    uint64_t rcvbuf_errors = 0;  // RcvbufErrors: 内核收包缓冲满被丢的 datagram 数
    uint64_t in_errors = 0;      // InErrors:   其他接收错误(含校验和等)
    bool valid = false;          // 是否成功解析(文件不存在/字段缺失时为 false)
};

inline std::vector<std::string> splitWs(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream ss(s);
    std::string tok;
    while (ss >> tok) out.push_back(tok);
    return out;
}

// 读取 /proc/net/snmp 中 Udp: 的 RcvbufErrors 与 InErrors。
// 格式是两行都以 "Udp:" 开头:第一行是列名,第二行是对应的数值。
inline UdpSnmp readUdpSnmp() {
    UdpSnmp snmp;
    std::ifstream f("/proc/net/snmp");
    std::string header, data, line;
    while (std::getline(f, line)) {
        if (line.rfind("Udp:", 0) == 0) {
            if (header.empty()) header = line;
            else { data = line; break; }
        }
    }
    if (data.empty()) return snmp;

    std::vector<std::string> h = splitWs(header);
    std::vector<std::string> d = splitWs(data);

    auto index_of = [&](const std::string& name) -> int {
        for (size_t i = 0; i < h.size(); i++) {
            if (h[i] == name) return static_cast<int>(i);
        }
        return -1;
    };

    int ri = index_of("RcvbufErrors");
    int ii = index_of("InErrors");
    if (ri < 0 || ii < 0) return snmp;
    if (d.size() <= static_cast<size_t>(std::max(ri, ii))) return snmp;

    snmp.rcvbuf_errors = strtoull(d[ri].c_str(), nullptr, 10);
    snmp.in_errors     = strtoull(d[ii].c_str(), nullptr, 10);
    snmp.valid = true;
    return snmp;
}

// 把订阅者 socket 的 SO_RCVBUF 设为 desired 字节,再用 getsockopt 回读实际生效值。
// Linux 会把 setsockopt(SO_RCVBUF) 传入的值翻倍,所以回读值通常是 desired 的 2 倍;
// 这里不回读就无从得知真实生效值,更不能假设 setsockopt 一定成功。
// 返回回读到的字节数;getsockopt 失败时返回 -1。
inline int setSubscriberRcvBuf(int fd, int desired) {
    int val = desired;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &val, sizeof(val));
    val = 0;
    socklen_t len = sizeof(val);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &val, &len) == 0) return val;
    return -1;
}

} // namespace bench

#endif // BENCH_COMMON_H
