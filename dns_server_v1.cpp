#include<iostream>
#include<cstring>
#include<sys/socket.h>
#include<netinet/in.h>
#include<unistd.h>

class UdpServer{
private:
    int server_fd; //Socket
public:
    //UDO构造函数：负责初始化Socket和Bind
    UdpServer(uint16_t port) {
        server_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if(server_fd < 0) {
            std::cerr << "Socket creation failed" <<std::endl;
            exit(EXIT_FAILURE);
        }
        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(port);
        if(bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0){
            std::cerr << "Bind failed" << std::endl;
            close(server_fd);
            exit(EXIT_FAILURE);
        }
        std::cout << "[DNS++ V1] Server initialized on port " << port << std::endl;
        
    }
    ~UdpServer() {
        if(server_fd >= 0 ) {
            close(server_fd);
            std::cout << "[DNS++ V1] Server socket closed safely. " << std::endl;
        }
    }
    void start() {
        char buffer[1024];
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        std::cout << "[DNS++ V1] Waiting for queries ... " <<std::endl;
        while(true){
            memset(buffer, 0, sizeof(buffer));

            int bytes_received = recvfrom(server_fd, buffer, sizeof(buffer) - 1, 0, 
                                           (struct sockaddr*)&client_addr, &client_len);
            if(bytes_received > 0){
                std::cout << "-> Received query; " << std::endl;

                std::string response = "DNS++ ACK: " + std::string(buffer);

                sendto(server_fd, response.c_str(), response.length(), 0,
                        (struct sockaddr*)&client_addr, client_len);
                        std::cout <<  "<- Sent response back to client. " << std::endl;
            }
        }
    }
};
int main() {

    UdpServer my_server(8080);

    my_server.start();
    return 0;
}

