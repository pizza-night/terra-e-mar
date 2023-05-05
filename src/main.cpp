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

    /* Peer(const Peer&) = delete; */
    /* Peer& operator=(const Peer&) = delete; */
    /* ~Peer() { close(this->socket); }; */

    int send_packet(const uint8_t* bytes, uint32_t size) {
        if (send(this->socket, bytes, size, 0) == -1) {
            std::perror("Failed send_packet");
            return -1;
        }
        return 0;
    }
};

class Peers {
  private:
    std::mutex lock;
    std::unordered_map<uint32_t, Peer> peers;

    int broadcast_packet(const uint8_t* bytes, uint32_t size) {
        std::unique_lock guard(this->lock);
        for (auto& pair : this->peers) {
            if (pair.second.send_packet(bytes, size) == -1) {
                return -1;
            }
        }

        return 0;
    }

  public:
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

    auto receive(int socket, void* buf, uint32_t n, int flags) -> int {
        int size = recv(socket, buf, n, flags);
        if (size == -1) {
            perror("There was a connection issue.");
            this->remove(socket);
            return -1;
        }
        if (size == 0) {
            perror("User disconnected");
            this->remove(socket);
            return -1;
        }

        return size;
    }

    void remove(int socket) {
        std::unique_lock guard(this->lock);
        this->peers.erase(this->peers.find(socket));
        close(socket);
    }

    int broadcast_message(std::string message) {
        std::vector<uint8_t> packet;
        packet.push_back(0);
        uint32_t size = htonl(message.size());

        uint8_t a[4];
        memcpy(a, &size, sizeof(size));
        packet.insert(packet.end(), a, a + 4);

        packet.insert(packet.end(), message.begin(), message.end());
        return this->broadcast_packet(packet.data(), packet.size());
    }

    int broadcast_username(std::string user) {
        std::vector<uint8_t> packet;
        packet.push_back(1);
        packet.push_back(user.length());
        packet.insert(packet.end(), user.begin(), user.end());
        return this->broadcast_packet(packet.data(), packet.size());
    }

    int send_known_peers(int socket) {
        std::vector<uint8_t> packet;
        packet.push_back(3);
        // TODO: generate packet
        return this->find(socket).send_packet(packet.data(), packet.size());
    }
};

int read_from_socket(Peers& known_peers, int socket) {

    while (true) {
        uint8_t packet_type[1];

        if (int size = known_peers.receive(socket, packet_type, 1, 0) != 1) {
            return -1;
        };

        switch (packet_type[0]) {
            case 0: {
                uint8_t packet_size[4];
                if (int size = known_peers.receive(socket, packet_size, 4, 0) != 4) {
                    return -1;
                };
                uint32_t packet_size_parsed = ntohl(*(uint32_t*) packet_size);

                std::vector<uint8_t> packet_message(packet_size_parsed + 1);

                if (int size =
                        known_peers.receive(socket, packet_message.data(), packet_size_parsed, 0) !=
                        packet_size_parsed) {
                    return -1;
                };

                packet_message[packet_size_parsed + 1] = 0;

                auto const& user = known_peers.find(socket);
                std::cout << ">" << user.username << ": " << packet_message.data() << "\n";

                break;
            }
            case 1: {
                char packet_size[1];
                int size = known_peers.receive(socket, packet_size, 1, 0);
                std::cout << "Username\n";
                break;
            }
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

        /* if (known_peers.send_known_peers(socket) == -1) { */
        /*     continue; */
        /* } */

        read_threads.emplace_back([&] { read_from_socket(known_peers, socket); });
    }
}

auto main(int argc, char** argv) -> int {

    int listening = socket(AF_INET, SOCK_STREAM, 0);

    if (listening == -1) {
        perror("Can't create a socket!");
        return -1;
    }

    int enable = 1;
    if (setsockopt(listening, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
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

    if (port != 2504) {
        int connecting = socket(AF_INET, SOCK_STREAM, 0);
        if (connecting == -1) {
            perror("Can't create a socket!");
            return -1;
        }

        struct sockaddr_in server_addr; // set server addr and port
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(2504);
        struct hostent* hostnm = gethostbyname("localhost");
        server_addr.sin_addr.s_addr = *((unsigned long*) hostnm->h_addr);

        if (connect(connecting, (struct sockaddr*) &server_addr, sizeof(server_addr)) == -1) {
            perror("Problem with client connecting!");
            return -1;
            ;
        }

        known_peers.insert(server_addr, connecting);

        read_threads.emplace_back([&] { read_from_socket(known_peers, connecting); });
    }

    std::thread t_accept([&] { accept_con(known_peers, listening); });

    std::string message;
    while (std::getline(std::cin, message)) {
        /* known_peers.broadcast_username(username); */

        known_peers.broadcast_message(message);
    }

    for (auto& t : read_threads) {
        t.join();
    }

    return 0;
}
