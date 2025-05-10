#pragma once

#include <string>
#include <memory>
#include <functional>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

using json = nlohmann::json;

class GameSocket {
public:
    GameSocket();
    ~GameSocket();

    bool bind(const std::string& host, int port);
    bool connect(const std::string& host, int port);
    bool listen(int backlog = 5);
    std::shared_ptr<GameSocket> accept();
    
    bool send(const json& data);
    bool receive(json& data);
    
    void close();
    bool is_valid() const;
    
    std::string get_address() const;
    int get_port() const;

private:
    #ifdef _WIN32
    SOCKET socket_;
    #else
    int socket_;
    #endif
    
    std::string address_;
    int port_;
    bool is_valid_;
    
    static constexpr int BUFFER_SIZE = 4096;
}; 