#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <thread>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>
#include <stdio.h>

// Device information structure
struct DeviceInfo {
    std::string name;
    std::string status;
    int fault_probability;  // Percentage chance of fault (0-100)
};

// System monitor class
class SystemMonitor {
private:
    std::vector<DeviceInfo> devices;
    std::mt19937 random_generator;
    std::uniform_int_distribution<int> random_dist;
    std::string web_server_host;
    int web_server_port;
    int notification_port;
    bool external_status_override;
    std::string external_system_status;
    
public:
    SystemMonitor(const std::string& host = "127.0.0.1", int port = 12345) 
        : random_generator(std::time(nullptr)), random_dist(1, 100),
          web_server_host(host), web_server_port(port), notification_port(54321),
          external_status_override(false) {
        printf("SystemMonitor constructor: Starting initialization\n");
        fflush(stdout);
        initialize_devices();
        printf("SystemMonitor constructor: Initialization complete\n");
        fflush(stdout);
    }
    
    void initialize_devices() {
        printf("initialize_devices: Starting\n");
        fflush(stdout);
        
        // Clear any existing devices
        devices.clear();
        printf("initialize_devices: Cleared devices\n");
        fflush(stdout);
        
        // Reserve space for better performance
        devices.reserve(6);
        printf("initialize_devices: Reserved space\n");
        fflush(stdout);
        
        try {
            // Initialize device list with different fault probabilities
            DeviceInfo dev1 = {"Device1", "ok", 5};
            devices.push_back(dev1);
            printf("initialize_devices: Added Device1\n");
            fflush(stdout);
            
            DeviceInfo dev2 = {"Device2", "ok", 15};
            devices.push_back(dev2);
            printf("initialize_devices: Added Device2\n");
            fflush(stdout);
            
            DeviceInfo dev3 = {"Device3", "ok", 3};
            devices.push_back(dev3);
            printf("initialize_devices: Added Device3\n");
            fflush(stdout);
            
            DeviceInfo dev4 = {"Network Controller", "operational", 8};
            devices.push_back(dev4);
            printf("initialize_devices: Added Network Controller\n");
            fflush(stdout);
            
            DeviceInfo dev5 = {"Storage Unit", "operational", 12};
            devices.push_back(dev5);
            printf("initialize_devices: Added Storage Unit\n");
            fflush(stdout);
            
            DeviceInfo dev6 = {"Comm Link", "active", 7};
            devices.push_back(dev6);
            printf("initialize_devices: Added Comm Link\n");
            fflush(stdout);
            
        } catch (...) {
            printf("initialize_devices: Exception caught during device creation\n");
            fflush(stdout);
            return;
        }
        
        printf("Initialized %d devices\n", (int)devices.size());
        fflush(stdout);
    }
    
    void start_notification_listener() {
        printf("Starting notification listener on port %d\n", notification_port);
        
        std::thread listener_thread([this]() {
            int server_sock = socket(AF_INET, SOCK_STREAM, 0);
            if (server_sock < 0) {
                printf("Failed to create notification listener socket\n");
                return;
            }
            
            // Set socket options
            int opt = 1;
            setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            
            sockaddr_in server_addr = {0};
            server_addr.sin_family = AF_INET;
            server_addr.sin_addr.s_addr = INADDR_ANY;
            server_addr.sin_port = htons(notification_port);
            
            if (bind(server_sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
                printf("Failed to bind notification listener socket\n");
                close(server_sock);
                return;
            }
            
            if (listen(server_sock, 5) < 0) {
                printf("Failed to listen on notification socket\n");
                close(server_sock);
                return;
            }
            
            printf("Notification listener ready on port %d\n", notification_port);
            
            while (true) {
                sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_sock = accept(server_sock, (sockaddr*)&client_addr, &client_len);
                
                if (client_sock < 0) {
                    continue;
                }
                
                // Handle notification in a separate thread
                std::thread([this, client_sock]() {
                    handle_notification(client_sock);
                    close(client_sock);
                }).detach();
            }
        });
        
        listener_thread.detach();
    }
    
    void handle_notification(int client_sock) {
        char buffer[4096];
        ssize_t bytes_received = recv(client_sock, buffer, sizeof(buffer)-1, 0);
        
        if (bytes_received <= 0) {
            return;
        }
        
        buffer[bytes_received] = '\0';
        std::string notification(buffer);
        
        printf("Received notification from webserver:\n%s\n", notification.c_str());
        
        // Parse the notification
        std::istringstream stream(notification);
        std::string line;
        
        while (std::getline(stream, line)) {
            if (line.find("SYSTEM_STATUS_UPDATE:") == 0) {
                printf("Debug: parsing line: '%s'\n", line.c_str());
                printf("Debug: line length: %d\n", (int)line.length());
                printf("Debug: 'SYSTEM_STATUS_UPDATE:' length: %d\n", (int)strlen("SYSTEM_STATUS_UPDATE:"));
                external_system_status = line.substr(21); // Remove "SYSTEM_STATUS_UPDATE:" (21 chars)
                printf("Debug: extracted status: '%s'\n", external_system_status.c_str());
                external_status_override = true;
                printf("Backend received system status override: '%s'\n", external_system_status.c_str());
            } else if (line.find("DEVICE:") == 0) {
                // Parse device update
                size_t eq_pos = line.find('=');
                if (eq_pos != std::string::npos) {
                    std::string device_name = line.substr(7, eq_pos - 7); // Remove "DEVICE:"
                    std::string device_status = line.substr(eq_pos + 1);
                    
                    // Update the device status in our list
                    for (auto& device : devices) {
                        if (device.name == device_name) {
                            device.status = device_status;
                            printf("Backend updated device '%s' to '%s'\n", device_name.c_str(), device_status.c_str());
                            break;
                        }
                    }
                }
            } else if (line == "END") {
                break;
            }
        }
    }
    
    void update_device_statuses() {
        // Get current timestamp
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        char timestamp[100];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&time_t));
        

        int fault_count = 0;
        
        for (auto& device : devices) {
            // Using C rand() instead of C++ random (for comparison):
            // int roll = (rand() % 100) + 1;  // Less uniform, thread-unsafe
            int roll = random_dist(random_generator);
            
            if (roll <= device.fault_probability) {
                // Device goes to fault state
                if (device.status != "fault") {
                    device.status = "fault";
                    printf("[%s] Device '%s' changed to FAULT\n", timestamp, device.name.c_str());
                }
                fault_count++;
            } else {
                // Device recovers to normal state
                std::string new_status;
                if (device.name.find("Controller") != std::string::npos ||
                    device.name.find("Unit") != std::string::npos) {
                    new_status = "operational";
                } else if (device.name.find("Link") != std::string::npos) {
                    new_status = "active";
                } else {
                    new_status = "ok";
                }
                
                if (device.status != new_status) {
                    device.status = new_status;
                    printf("[%s] Device '%s' recovered to %s\n", timestamp, device.name.c_str(), new_status.c_str());
                }
            }
        }
        
        // Update overall system status
        std::string overall_status = "Operational";
        
        if (external_status_override) {
            // Use the status set from webserver
            overall_status = external_system_status;
            printf("[%s] Using external system status: '%s'\n", timestamp, overall_status.c_str());
            // Clear the override after one use (optional - remove this if you want it to persist)
            // external_status_override = false;
        } else {
            // Generate status based on device faults
            if (fault_count > 0) {
                if (fault_count == 1) {
                    overall_status = "Warning: 1 device fault";
                } else {
                    overall_status = "Critical: " + std::to_string(fault_count) + " device faults";
                }
            } else {
                overall_status = "Operational";
            }
            printf("[%s] Generated system status: '%s'\n", timestamp, overall_status.c_str());
        }
        
        // Send update to web server
        send_status_update(overall_status);
    }
    
    void send_status_update(const std::string& system_status) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("socket creation failed");
            return;
        }
        
        sockaddr_in server_addr = {0};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(web_server_port);
        inet_pton(AF_INET, web_server_host.c_str(), &server_addr.sin_addr);
        
        if (connect(sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            printf("Connection to web server failed (server may not be running)\n");
            close(sock);
            return;
        }
        
        // Create POST request body with device statuses
        std::ostringstream post_body;
        std::string encoded_status = url_encode(system_status);
        printf("Encoding system status: '%s' -> '%s'\n", system_status.c_str(), encoded_status.c_str());
        post_body << "system_status=" << encoded_status;
        
        for (const auto& device : devices) {
            post_body << "&" << url_encode(device.name) << "=" << url_encode(device.status);
        }
        
        std::string body = post_body.str();
        printf("POST body: %s\n", body.c_str());
        
        // Create HTTP POST request
        std::ostringstream request;
        request << "POST /update_system HTTP/1.1\r\n";
        request << "Host: " << web_server_host << ":" << web_server_port << "\r\n";
        request << "Content-Type: application/x-www-form-urlencoded\r\n";
        request << "Content-Length: " << body.length() << "\r\n";
        request << "Connection: close\r\n";
        request << "\r\n";
        request << body;
        
        std::string http_request = request.str();
        
        // Send the request
        ssize_t sent = send(sock, http_request.c_str(), http_request.length(), 0);
        if (sent < 0) {
            perror("send failed");
        } else {
            printf("Status update sent to web server (%d bytes)\n", (int)sent);
        }
        
        close(sock);
    }
    
    std::string url_encode(const std::string& str) {
        std::ostringstream encoded;
        for (char c : str) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                encoded << c;
            } else if (c == ' ') {
                encoded << '+';
            } else {
                // Use sprintf for guaranteed correct hex encoding
                char hex_buffer[4];
                sprintf(hex_buffer, "%%%02X", (unsigned char)c);
                encoded << hex_buffer;
            }
        }
        return encoded.str();
    }
    
    void run() {
        printf("System Monitor started\n");
        printf("Monitoring %d devices, updating every 5 seconds\n", (int)devices.size());
        printf("Web server: %s:%d\n", web_server_host.c_str(), web_server_port);
        
        // Start the notification listener
        start_notification_listener();
        
        while (true) {
            update_device_statuses();
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
    
    void print_current_status() {
        printf("\n=== Current Device Status ===\n");
        for (const auto& device : devices) {
            printf("%-20s: %s\n", device.name.c_str(), device.status.c_str());
        }
        printf("=============================\n\n");
    }
};

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    int port = 12345;
    
    printf("Starting System Monitor Backend\n");
    fflush(stdout);
    
    // Parse command line arguments
    if (argc >= 2) {
        host = argv[1];
    }
    if (argc >= 3) {
        port = std::atoi(argv[2]);
    }
    
    printf("Target web server: %s:%d\n", host.c_str(), port);
    fflush(stdout);
    
    printf("Creating SystemMonitor object...\n");
    fflush(stdout);
    
    SystemMonitor monitor(host, port);
    
    printf("SystemMonitor object created successfully\n");
    fflush(stdout);
    
    // Print initial status
    monitor.print_current_status();
    
    // Start monitoring loop
    monitor.run();
    
    return 0;
}
