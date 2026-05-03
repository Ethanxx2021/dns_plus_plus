#include<iostream>
#include<vector>
#include<string>

int main() {
    // 1.模拟DNS++节点启动，加载路由表
    std::vector<std::string> routing_table = {"192.168.1.10", "10.0.0.5" , "8.8.8.8"};

    std::cout << "[DNS++ Node] Booting up ... Loaded upstream proxies:" << std::endl;

    //const auto&范围for循环，避免拷贝字符串，提升性能

    for(const auto& ip : routing_table){
        std::cout<< "->" << ip << std::endl;
    }

    //2.模拟节点更新 引用&
    int active_queries = 0;
    int& ref_queries = active_queries;

    ref_queries = 5; //当五个DNS到达时，通过引用修改

    std::cout << "[DNS++ Node] Current active queries:" << active_queries << std::endl;
    return 0;

}