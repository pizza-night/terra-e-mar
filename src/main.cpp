#include <algorithm>
#include <arpa/inet.h>
#include <iostream>
#include <mutex>
#include <netdb.h>
#include <optional>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

std::string username = "jff";

class Peer {
  private:
    sockaddr_in addr;
    int socket;

    void generate_username() {
        this->username = inet_ntoa(this->addr.sin_addr);
        this->username.append(":");
        this->username.append(std::to_string(this->addr.sin_port));
    }

  public:
    std::string username;

    Peer(sockaddr_in addr, int socket): addr(addr), socket(socket) { generate_username(); }

    /* ~Peer() { */
    /*     close(this->socket); */
    /* }; */

    int send_packet(const char* bytes, uint32_t size) {
        if (send(this->socket, bytes, size + 1, 0) == -1) {
            std::perror("Failed send_packet");
            return -1;
        }
        return 0;
    }
};

class Peers {
  private:
    int broadcast_packet(const char* bytes, uint32_t size) {
        std::unique_lock guard(this->lock);
        for (auto& pair : this->peers) {
            if (pair.second.send_packet(bytes, size) == -1) {
                return -1;
            }
        }

        return 0;
    }

  public:
    std::mutex lock;
    std::unordered_map<uint32_t, Peer> peers;

    void insert(sockaddr_in addr, int socket) {
        std::unique_lock guard(this->lock);
        this->peers.emplace(socket, Peer(addr, socket));
    }

    auto find(int socket) -> Peer& {
        std::unique_lock guard(this->lock);

        auto search = this->peers.find(socket);
        if (search == this->peers.end()) abort();

        return search->second;
    }

    void remove(int socket) {
        std::unique_lock guard(this->lock);
        this->peers.erase(this->peers.find(socket));
    }

    int broadcast_message(std::string message) {
        std::vector<char> packet;
        packet.push_back(0);
        // TODO: fix message size
        packet.push_back(message.length());
        packet.insert(packet.end(), message.begin(), message.end());
        return this->broadcast_packet(packet.data(), packet.size());
    }

    int broadcast_username(std::string user) {
        std::vector<char> packet;
        packet.push_back(1);
        packet.push_back(user.length());
        packet.insert(packet.end(), user.begin(), user.end());
        return this->broadcast_packet(packet.data(), packet.size());
    }

    int send_known_peers(int socket) {
        std::vector<char> packet;
        packet.push_back(3);
        // TODO: generate packet
        return this->find(socket).send_packet(packet.data(), packet.size());
    }
};

int read_from_socket(Peers& known_peers, int socket) {

    char buf[4096];
    while (true) {
        memset(buf, 0, 4096);
        int size = recv(socket, buf, 4096, 0);
        if (size == -1) {
            perror("There was a connection issue.");
            known_peers.remove(socket);
            return -1;
        }
        if (size == 0) {
            perror("The client disconnected");
            known_peers.remove(socket);
            return 0;
        }

        // display message

        auto const& user = known_peers.find(socket);
        std::cout << "Received " << user.username << ": " << std::string(buf, 0, size);

        switch (buf[0]) {
            case 0:
                std::cout << "Message\n";
                break;
            case 1:
                std::cout << "Username\n";
                break;
            case 2:
                std::cout << "Initial\n";
                break;
            default:
                break;
        }
    }

    known_peers.remove(socket);

    return 1;
}

std::vector<std::thread> read_threads;

void accept_con(Peers& known_peers, int listening) {
    sockaddr_in client;
    socklen_t client_size = sizeof(client);

    while (int socket = accept(listening, (struct sockaddr*) &client, &client_size)) {

        if (socket == -1) {
            perror("Problem with client connecting!");
            continue;
        }

        known_peers.insert(client, socket);
        std::cout << "New Peer Connected: " << known_peers.find(socket).username << "\n";

        if (known_peers.send_known_peers(socket) == -1) {
            continue;
        }

        read_threads.emplace_back([&] { read_from_socket(known_peers, socket); });
    }
}

auto main(int argc, char** argv) -> int {

    int listening = socket(AF_INET, SOCK_STREAM, 0);
    if (listening == -1) {
        perror("Can't create a socket!");
        return -1;
    }

    int port = (argc > 1) ? atoi(argv[1]) : 2504;

    struct sockaddr_in hint;
    hint.sin_family = AF_INET;
    hint.sin_port = htons(port);
    inet_pton(AF_INET, "0.0.0.0", &hint.sin_addr);

    if (bind(listening, (struct sockaddr*) &hint, sizeof(hint)) == -1) {
        std::perror("Can't bind to IP/port");
        return -2;
    }

    if (listen(listening, SOMAXCONN) == -1) {
        std::perror("Can't listen !");
        return -3;
    }

    Peers known_peers;

    /* if (port != 2504) { */
    /*     struct sockaddr_in server_addr; // set server addr and port */
    /*     server_addr.sin_family = AF_INET; */
    /*     server_addr.sin_port = htons(2504); */
    /*     inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr); */


    /*     int socket = connect(listening, (struct sockaddr*) &server_addr, sizeof(server_addr)); */
    /*     if (socket == -1) { */
    /*         perror("Problem with client connecting!"); */
    /*         return -1;; */
    /*     } */

    /*     known_peers.insert(server_addr, socket); */

    /*     std::string bytes("qweqweqweqwe"); */

    /*     if (send(socket, bytes.c_str(), bytes.length() + 1, 0) == -1) { */
    /*         std::perror("Failed send_packet"); */
    /*         return -1; */
    /*     } */
    /*     std::cout << known_peers.peers.size() << "\n"; */
    /* } */

    std::thread t_accept([&] { accept_con(known_peers, listening); });

    while (true) {
        std::string message;
        std::cin >> message;

        std::cout << known_peers.peers.size() << "\n";

        /* known_peers.broadcast_username(username); */
        known_peers.broadcast_message(message);
    }

    for (auto& t : read_threads) {
        t.join();
    }

    return 0;
}
