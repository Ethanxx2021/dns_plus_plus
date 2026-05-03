#include<iostream>
#include<cstring>
#include<sys/socket.h>
#include<netinet/in.h>
#include<unistd.h>

int main() {
    //  创建UDP Socket
    int server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_fd < 0 ) {
        std::cerr <<"Failed to create socket" << std::endl;
        return -1;
    }
    // 2.配置服务器地址结构体
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr)); //清零
    server_addr.sin_family = AF_INET;             //IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY;     //监听本机所有网卡（0.0.0.0）
    server_addr.sin_port = htons(8080);           //端口8080，必须用htons转为网络大端序
    // 3.绑定socket和地址
    if(bind(server_fd,(struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Build failed. Port might be in use." << std::endl;
        return -1;
    }
    std::cout << "[DNS++ Node] UDP server listening on port 8080 ..." << std::endl;
    //4.准备接收数据的缓冲区和客户端地址结构体
    char buffer[1024];
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    //5.死循环接收数据
    while(true) {
        memset(buffer, 0, sizeof(buffer));  //每次接受前清空缓存区

        int bytes_received = recvfrom(server_fd,buffer, sizeof(buffer) - 1, 0,
                                                        (struct sockaddr*)&client_addr, &client_len);
        if(bytes_received > 0) {
            std::cout << "Received" << bytes_received << " bytes: " << buffer << std::endl;
        }
    }
    close(server_fd);
    return 0;


}