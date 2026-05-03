#include<iostream>
#include<unistd.h>

class Dnsnode {
private:
    int fd;
public:
    //1.构造函数（constructor）：创建对象时自动调用
    Dnsnode() {
        fd = 999;
        std::cout << "[DnsNode] Constructor called. Resource acquired: " << fd << std::endl;
    }
    //2.析构函数（Destructor）：对象被销毁时自动调用
    ~Dnsnode() {
        std::cout << "[DnsNode] Destructor called. Resource released: " << std::endl;
    }
    //3.普通成员方法
    void doWork(){
        std::cout << "[DnsNode] Node is processing DNS queries ... " << std::endl;
    }
};

int main() {
    std::cout << "--- Main starts ---" << std::endl;
    {
        //作用域内创建对象
        Dnsnode node;
        node.doWork();
    }
    std::cout << "--- Main ends ---" << std::endl;
    return 0;
}