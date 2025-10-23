#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netdb.h>
#include <string.h>
#include <string>
#include <fstream>
#include <iostream>
#include <errno.h>

// Handle SIGCHLD to prevent zombie processes
void handle_sigchld(int sig) {
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

// Parse IP:port from command-line argument
bool parse_ip_port(const char* input, std::string& ip, std::string& port) {
    std::string input_str(input);
    size_t colon_pos = input_str.find(':');
    if (colon_pos == std::string::npos) return false;
    ip = input_str.substr(0, colon_pos);
    port = input_str.substr(colon_pos + 1);
    try {
        int port_num = std::stoi(port);
        if (port_num < 1024 || port_num > 65535) return false;
    } catch (...) {
        return false;
    }
    return true;
}

// Count slashes in URL
int count_slashes(const std::string& url) {
    int count = 0;
    for (char c : url) if (c == '/') count++;
    return count;
}

// Handle client request
void handle_client(int client_sock) {
    char buffer[1024];
    ssize_t bytes_read = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0) {
        close(client_sock);
        return;
    }
    buffer[bytes_read] = '\0';

    // Parse HTTP request
    std::string request(buffer);
    std::string method, url;
    size_t pos = request.find(' ');
    if (pos == std::string::npos) {
        std::string response = "HTTP/1.1 400 Bad Request\r\n\r\n";
        send(client_sock, response.c_str(), response.length(), 0);
        close(client_sock);
        return;
    }
    method = request.substr(0, pos);
    size_t pos2 = request.find(' ', pos + 1);
    if (pos2 == std::string::npos) {
        std::string response = "HTTP/1.1 400 Bad Request\r\n\r\n";
        send(client_sock, response.c_str(), response.length(), 0);
        close(client_sock);
        return;
    }
    url = request.substr(pos + 1, pos2 - pos - 1);

    // Validate URL
    if (count_slashes(url) > 3 || url.find("..") != std::string::npos) {
        std::string response = "HTTP/1.1 403 Forbidden\r\n\r\n";
        send(client_sock, response.c_str(), response.length(), 0);
        close(client_sock);
        return;
    }

    // Extract filename (remove leading '/')
    std::string filename = url.substr(1);
    if (filename.empty()) filename = "index.html";

    // Open file
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::string response = "HTTP/1.1 404 Not Found\r\n\r\n";
        send(client_sock, response.c_str(), response.length(), 0);
        close(client_sock);
        return;
    }

    // Get file size
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    // Prepare headers
    std::string headers = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(file_size) +
                         "\r\nContent-Type: text/plain\r\n\r\n";

    // Send headers
    send(client_sock, headers.c_str(), headers.length(), 0);

    // For GET, send file content
    if (method == "GET") {
        char file_buffer[1024];
        while (file.read(file_buffer, sizeof(file_buffer))) {
            send(client_sock, file_buffer, file.gcount(), 0);
        }
        if (file.gcount() > 0) {
            send(client_sock, file_buffer, file.gcount(), 0);
        }
    }

    file.close();
    close(client_sock);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s IP:PORT\n", argv[0]);
        return 1;
    }

    std::string ip, port;
    if (!parse_ip_port(argv[1], ip, port)) {
        fprintf(stderr, "Invalid IP:PORT format\n");
        return 1;
    }

    // Set up SIGCHLD handler
    struct sigaction sa;
    sa.sa_handler = handle_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);

    // Set up socket
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int status = getaddrinfo(ip.c_str(), port.c_str(), &hints, &res);
    if (status != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
        return 1;
    }

    int server_sock = -1;
    for (p = res; p != NULL; p = p->ai_next) {
        server_sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (server_sock < 0) {
            perror("socket");
            continue;
        }

        // Allow port reuse
        int opt = 1;
        if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            perror("setsockopt");
            close(server_sock);
            continue;
        }

        if (bind(server_sock, p->ai_addr, p->ai_addrlen) < 0) {
            perror("bind");
            close(server_sock);
            continue;
        }

        break; // Successfully bound
    }

    if (p == NULL) {
        fprintf(stderr, "Failed to bind to any address\n");
        freeaddrinfo(res);
        return 1;
    }

    freeaddrinfo(res);

    if (listen(server_sock, 10) < 0) {
        perror("listen");
        close(server_sock);
        return 1;
    }

    printf("Server running on %s:%s\n", ip.c_str(), port.c_str());

    // Main loop
    while (true) {
        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &addr_len);
        if (client_sock < 0) {
            perror("accept");
            continue;
        }

        // Fork to handle client
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(client_sock);
            continue;
        }
        if (pid == 0) { // Child process
            close(server_sock);
            handle_client(client_sock);
            exit(0);
        }
        close(client_sock);
    }

    close(server_sock);
    printf("done.\n");
    return 0;
}