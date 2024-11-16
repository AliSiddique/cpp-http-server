#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <map>
#include <ctime>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <filesystem>
#include <thread>
#include <vector>
#include <signal.h>

class HTTPServer {
private:
    int server_fd;
    int port;
    std::string web_root;
    bool running;
    std::vector<std::thread> client_threads;
    std::map<std::string, std::string> mime_types;

    void setupMimeTypes() {
        mime_types = {
            {".html", "text/html"},
            {".css", "text/css"},
            {".js", "application/javascript"},
            {".json", "application/json"},
            {".png", "image/png"},
            {".jpg", "image/jpeg"},
            {".jpeg", "image/jpeg"},
            {".gif", "image/gif"},
            {".txt", "text/plain"}
        };
    }

    std::string getMimeType(const std::string& path) {
        size_t dot_pos = path.find_last_of('.');
        if (dot_pos != std::string::npos) {
            std::string ext = path.substr(dot_pos);
            auto it = mime_types.find(ext);
            if (it != mime_types.end()) {
                return it->second;
            }
        }
        return "application/octet-stream";
    }

    std::string getTimeString() {
        time_t now = time(0);
        struct tm tm = *gmtime(&now);
        char buf[100];
        strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
        return std::string(buf);
    }

    void handleClient(int client_socket) {
        char buffer[4096] = {0};
        ssize_t bytes_read = read(client_socket, buffer, 4096);
        
        if (bytes_read > 0) {
            std::string request(buffer);
            std::istringstream request_stream(request);
            std::string request_line;
            std::getline(request_stream, request_line);

            // Parse request line
            std::istringstream request_line_stream(request_line);
            std::string method, path, protocol;
            request_line_stream >> method >> path >> protocol;

            if (method == "GET") {
                handleGetRequest(client_socket, path);
            } else {
                // Method not supported
                std::string response = "HTTP/1.1 405 Method Not Allowed\r\n"
                                     "Content-Type: text/plain\r\n"
                                     "Content-Length: 21\r\n\r\n"
                                     "Method Not Supported\n";
                send(client_socket, response.c_str(), response.length(), 0);
            }
        }

        close(client_socket);
    }

    void handleGetRequest(int client_socket, const std::string& path) {
        // Convert URL path to file system path
        std::string file_path = web_root + (path == "/" ? "/index.html" : path);
        
        // Security check: Prevent directory traversal
        std::filesystem::path canonical_path = std::filesystem::canonical(std::filesystem::path(web_root));
        std::filesystem::path requested_path = std::filesystem::canonical(std::filesystem::path(file_path));
        
        if (requested_path.string().find(canonical_path.string()) != 0) {
            sendError(client_socket, 403, "Forbidden");
            return;
        }

        // Check if file exists and is readable
        std::ifstream file(file_path, std::ios::binary);
        if (!file) {
            sendError(client_socket, 404, "Not Found");
            return;
        }

        // Get file size
        file.seekg(0, std::ios::end);
        size_t file_size = file.tellg();
        file.seekg(0, std::ios::beg);

        // Prepare and send headers
        std::ostringstream headers;
        headers << "HTTP/1.1 200 OK\r\n"
                << "Content-Type: " << getMimeType(file_path) << "\r\n"
                << "Content-Length: " << file_size << "\r\n"
                << "Date: " << getTimeString() << "\r\n"
                << "Server: CPP-HTTP-Server/1.0\r\n"
                << "Connection: close\r\n\r\n";

        std::string header_str = headers.str();
        send(client_socket, header_str.c_str(), header_str.length(), 0);

        // Send file content
        char buffer[4096];
        while (file.read(buffer, sizeof(buffer)).gcount() > 0) {
            send(client_socket, buffer, file.gcount(), 0);
        }
    }

    void sendError(int client_socket, int error_code, const std::string& error_message) {
        std::string body = "<html><body><h1>" + std::to_string(error_code) + " " + error_message + "</h1></body></html>";
        
        std::ostringstream response;
        response << "HTTP/1.1 " << error_code << " " << error_message << "\r\n"
                << "Content-Type: text/html\r\n"
                << "Content-Length: " << body.length() << "\r\n"
                << "Date: " << getTimeString() << "\r\n"
                << "Server: CPP-HTTP-Server/1.0\r\n"
                << "Connection: close\r\n\r\n"
                << body;

        std::string response_str = response.str();
        send(client_socket, response_str.c_str(), response_str.length(), 0);
    }

public:
    HTTPServer(int port = 8080, const std::string& web_root = "./www")
        : port(port), web_root(web_root), running(false) {
        setupMimeTypes();
    }

    ~HTTPServer() {
        stop();
    }

    void start() {
        // Create socket
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            throw std::runtime_error("Failed to create socket");
        }

        // Set socket options
        int opt = 1;
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            throw std::runtime_error("Failed to set socket options");
        }

        // Bind socket
        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);

        if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
            throw std::runtime_error("Failed to bind socket");
        }

        // Listen for connections
        if (listen(server_fd, 10) < 0) {
            throw std::runtime_error("Failed to listen");
        }

        running = true;
        std::cout << "Server started on port " << port << std::endl;
        std::cout << "Serving files from " << web_root << std::endl;

        while (running) {
            struct sockaddr_in client_address;
            socklen_t client_len = sizeof(client_address);
            
            int client_socket = accept(server_fd, (struct sockaddr*)&client_address, &client_len);
            if (client_socket < 0) {
                if (running) {
                    std::cerr << "Failed to accept connection" << std::endl;
                }
                continue;
            }

            // Handle client in a new thread
            client_threads.emplace_back(&HTTPServer::handleClient, this, client_socket);
        }

        // Wait for all client threads to finish
        for (auto& thread : client_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    void stop() {
        running = false;
        close(server_fd);
    }
};

// Signal handler for graceful shutdown
HTTPServer* global_server = nullptr;
void signal_handler(int signum) {
    if (global_server) {
        std::cout << "\nShutting down server..." << std::endl;
        global_server->stop();
    }
}

int main(int argc, char* argv[]) {
    try {
        int port = (argc > 1) ? std::stoi(argv[1]) : 8080;
        std::string web_root = (argc > 2) ? argv[2] : "./www";

        // Set up signal handling
        global_server = new HTTPServer(port, web_root);
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);

        // Create web root directory if it doesn't exist
        std::filesystem::create_directories(web_root);

        // Create a sample index.html if it doesn't exist
        std::string index_path = web_root + "/index.html";
        if (!std::filesystem::exists(index_path)) {
            std::ofstream index_file(index_path);
            index_file << "<html>\n"
                      << "<head><title>Welcome</title></head>\n"
                      << "<body>\n"
                      << "<h1>Welcome to CPP HTTP Server</h1>\n"
                      << "<p>Server is running successfully!</p>\n"
                      << "</body>\n"
                      << "</html>";
        }

        global_server->start();
        delete global_server;
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}