#include<iostream>
#include<cstring>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<unistd.h>
//1.定义DNS协议头 12字节
struct DnsHeader {
    uint16_t transaction_id;
    uint16_t flags;
    uint16_t questions;
    uint16_t answer_rrs;
    uint16_t authority_rrs;
    uint16_t additional_rrs;
};
class UdpServer {
private:
    int server_fd;
public:
    UdpServer(uint16_t port) {
        server_fd = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(port);
        bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
        std::cout << "[DNS++ V2] Binary Server listening on port" << port << std::endl;
    }
    ~UdpServer() {
        close(server_fd);
    }
    void start() {
        char buffer[1024];
        struct sockaddr_in client_addr;
        socklen_t cilent_len = sizeof(client_addr);
        while (true) {
            memset(buffer, 0, sizeof(buffer));

            int bytes = recvfrom(server_fd, buffer, sizeof(buffer), 0,
                                    (struct sockaddr*)&client_addr, &cilent_len);
            
                                    if(bytes > 0) {
                                        //1.升级1：解析并打印客户端的IP和PORT
                                        // inet_ntoa：把网络二进制IP转换成字符串
                                        // ntohs：把网络大端序端口转成本机小端序
                                        char* client_ip = inet_ntoa(client_addr.sin_addr);
                                        uint16_t client_port = ntohs(client_addr.sin_port);
                                        std::cout << "\n[+] Received " << bytes << "bytes from "
                                                  << client_ip << ":" << client_port << std::endl;
                                        //2.升级2：解析二进制DNS头部
                                        //只有当收到的数据大于等于十二字节，才是合法DNS头部
                                        if(bytes >= sizeof(DnsHeader)) {
                                            //核心：指针强转，把buffer当做DnsHeader
                                            DnsHeader* header = (DnsHeader*)buffer;
                                            //读取数据时，必须用ntohs转回本机字节序
                                            uint16_t tx_id = ntohs(header->transaction_id);
                                            uint16_t flags = ntohs(header->flags);
                                            uint16_t questions = ntohs(header->questions);
                                            std::cout << "  -> Transactions ID : 0x" << std::hex << tx_id << std::dec << std::endl;
                                            std::cout << "  -> Flags           : 0x" << std::hex << flags << std::dec << std::endl;
                                            std::cout << "  -> Questions       : 0x" << questions << std::endl;
                                        }else{
                                            std::cout << "  ->Data too short to be a DNS packet." <<std::endl;
                                        }
                                    }
        }
    }
};
int main() {
    UdpServer server(8080);
    server.start();
    return 0;
}
