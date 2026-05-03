#include<iostream>
#include<cstdint> // 引入 unit16_t等固定大小的整数类型
//定义协议头部
//为什么用unit16_t 网络协议对字节要求极其严格
//为什么不用int，因为int可能是4字节或者8字节，在不同系统上

struct DnsHeader
{
    uint16_t transaction_id;
    uint16_t flags;
    uint16_t questions;
    uint16_t answer_rrs;
    uint16_t authority_rrs;
    uint16_t additional_rrs;
};
int main() {
    DnsHeader header;
    header.transaction_id = 1234;
    header.flags = 0x0100;

    std::cout<< "[DNS++ Core] DnsHeader size:" << sizeof(DnsHeader) << "bytes." << std::endl;

    if(sizeof(DnsHeader)==12)
    {
        std::cout<< "Success! Header size is correct." <<std::endl;
    }
    return 0;
}
