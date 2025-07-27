#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <map>
#include <sstream>
#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <stdio.h>
#include <algorithm>
#include <cctype>
#include <sys/resource.h>
#include <vector>
#include <chrono>
#include <ctime>
#include <sys/select.h>

// Server type enumeration, for logging
enum ServerType {
    BACKEND_SERVER,
    WEB_SERVER
};

// URL decoding for form data
std::string url_decode(const std::string &src) {
    std::string result;
    for (size_t i = 0; i < src.length(); i++) {
        if (src[i] == '+') {
            result += ' ';
        } else if (src[i] == '%' && i+2 < src.length()) {
            char hex[3] = {src[i+1], src[i+2], '\0'};
            char decoded_char = static_cast<char>(strtol(hex, nullptr, 16));
            result += decoded_char;
            i += 2;
        } else {
            result += src[i];
        }
    }
    return result;
}

// Trim whitespace from string
std::string trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t");
    return str.substr(start, end - start + 1);
}

// Convert string to lowercase
std::string to_lower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return result;
}

// HTTP request structure
struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::string connection_header;
    std::string content_length_header;
    std::string body;
    bool keep_alive;
    ServerType server_type;  // Which server received this request
};

// Device status structure
struct DeviceStatus {
    std::string name;
    std::string status;
};

// Context structure for shared data
struct ThreadContext {
    pthread_mutex_t mutex;         // For system_status and app_vars
    pthread_mutex_t conn_mutex;    // For active_connections tracking
    int active_backend_connections;
    int active_web_connections;
    std::string system_status;
    std::map<std::string, std::string> app_vars;
    std::vector<DeviceStatus> device_statuses; // Device status list
};

// Thread arguments (per-client)
struct ClientThreadArgs {
    ThreadContext* ctx;    // Shared context
    int client_fd;         // Client socket
    int connection_id;     // Unique connection ID
    ServerType server_type; // Which server this client connected to
};


// Parse HTTP request from buffer
HttpRequest parse_http_request(const char* buffer, ssize_t bytes, int client_fd, ServerType server_type) {
    HttpRequest request = {};
    request.server_type = server_type;
    
    // Parse first line using getline to properly handle line endings
    std::istringstream req_stream(buffer);
    std::string request_line;
    std::getline(req_stream, request_line);
    
    // Remove trailing CR if present
    if (!request_line.empty() && request_line[request_line.size()-1] == '\r') {
        request_line.resize(request_line.size()-1);
    }

    // Parse components from cleaned request line
    std::istringstream line_stream(request_line);
    line_stream >> request.method >> request.path >> request.version;

    // Parse headers
    std::string line;
    while (std::getline(req_stream, line)) {
        // Remove trailing CR if present
        if (!line.empty() && line[line.size()-1] == '\r') {
            line.resize(line.size()-1);
        }

        // Empty line marks end of headers
        if (line.empty()) {
            break;
        }

        // Parse header
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string header_name = trim(line.substr(0, colon_pos));
            std::string header_value = trim(line.substr(colon_pos + 1));

            // Convert to lowercase for case-insensitive comparison
            header_name = to_lower(header_name);

            if (header_name == "connection") {
                request.connection_header = to_lower(header_value);
            } else if (header_name == "content-length") {
                request.content_length_header = header_value;
            }
        }
    }

    // Determine keep-alive
    if (request.version.substr(0, 8) == "HTTP/1.1") {
        // HTTP/1.1 defaults to keep-alive unless "close" is specified
        request.keep_alive = (request.connection_header != "close");
    } else if (request.version.substr(0, 8) == "HTTP/1.0") {
        // HTTP/1.0 requires explicit "keep-alive"
        request.keep_alive = (request.connection_header == "keep-alive");
    }

    // Extract body if present (for POST requests)
    if (request.method == "POST" && !request.content_length_header.empty()) {
        size_t content_length = std::stoul(request.content_length_header);
        size_t body_start = std::string(buffer).find("\r\n\r\n");

        if (body_start != std::string::npos) {
            body_start += 4;  // Skip past CRLFCRLF
            request.body = std::string(buffer).substr(body_start);

            // If we didn't get the full body, read the remaining bytes
            if (request.body.length() < content_length) {
                size_t remaining = content_length - request.body.length();
                char body_buffer[remaining + 1];
                ssize_t body_bytes = recv(client_fd, body_buffer, remaining, 0);
                if (body_bytes > 0) {
                    body_buffer[body_bytes] = '\0';
                    request.body += std::string(body_buffer, body_bytes);
                }
            }
        }
    }

    return request;
}

// Generate response for root path "/"
std::string handle_root_request(ThreadContext* ctx) {
    printf("[WEB] Serving root page request\n");
    
    pthread_mutex_lock(&ctx->mutex);
    std::string system_status = ctx->system_status;
    std::vector<DeviceStatus> devices = ctx->device_statuses; // Copy for thread safety
    pthread_mutex_unlock(&ctx->mutex);

    printf("[WEB] Current device count: %d\n", (int)devices.size());
    printf("[WEB] System status: %s\n", system_status.c_str());

    std::ostringstream html;
    html << "<html><head>"
         << "<title>COMM SYSTEM STATUS</title>"
         << "<style>"
         << "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f5f5f5; }"
         << ".container { max-width: 800px; margin: 0 auto; background: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }"
         << "h1 { color: #2c3e50; text-align: center; border-bottom: 2px solid #3498db; padding-bottom: 10px; }"
         << ".status-summary { background: #ecf0f1; padding: 15px; border-radius: 5px; margin: 20px 0; text-align: center; }"
         << ".status-summary h2 { margin: 0; color: #2c3e50; }"
         << ".status-value { font-size: 1.2em; font-weight: bold; color: #27ae60; }"
         << "table { width: 100%; border-collapse: collapse; margin-top: 20px; }"
         << "th { background-color: #3498db; color: white; text-align: left; padding: 12px; }"
         << "td { padding: 12px; border-bottom: 1px solid #ddd; }"
         << "tr:nth-child(even) { background-color: #f2f2f2; }"
         << ".ok { color: #27ae60; font-weight: bold; }"
         << ".fault { color: #e74c3c; font-weight: bold; }"
         << ".operational { color: #2980b9; font-weight: bold; }"
         << ".degraded { color: #f39c12; font-weight: bold; }"
         << ".active { color: #16a085; font-weight: bold; }"
         << ""
         << "/* Form Styling */"
         << ".update-form {"
         << "  background-color: #f8f9fa;"
         << "  border: 1px solid #dee2e6;"
         << "  border-radius: 8px;"
         << "  padding: 20px;"
         << "  margin: 20px 0;"
         << "}"
         << ".update-form h3 {"
         << "  margin: 0 0 15px 0;"
         << "  color: #343a40;"
         << "  font-size: 1.1em;"
         << "}"
         << ".form-row {"
         << "  display: flex;"
         << "  align-items: center;"
         << "  gap: 10px;"
         << "  flex-wrap: wrap;"
         << "}"
         << ".form-row input, .form-row select {"
         << "  border: 1px solid #ced4da;"
         << "  border-radius: 4px;"
         << "  font-size: 14px;"
         << "}"
         << ".form-row button:hover {"
         << "  opacity: 0.9;"
         << "  transform: translateY(-1px);"
         << "}"
         << "</style>\n"
         << "<script>\n"
         << "console.log('JavaScript loading...');\n"
         << "console.log('About to define refreshStatus function...');\n"
         << "function refreshStatus() {\n"
         << "  console.log('refreshStatus() called - about to fetch /check_status');\n"
         << "  var fetchPromise = fetch('/check_status');\n"
         << "  console.log('fetch() called, promise object:', fetchPromise);\n"
         << "  fetchPromise\n"
         << "    .then(function(r) { \n"
         << "      console.log('refreshStatus: response received:', r.status, r.statusText);\n"
         << "      return r.json(); \n"
         << "    })\n"
         << "    .then(function(data) { \n"
         << "      console.log('refreshStatus: JSON data received:', data);\n"
         << "      document.getElementById('status-value').innerText = data.status; \n"
         << "      document.getElementById('last-updated').innerText = data.timestamp;\n"
         << "      console.log('refreshStatus: DOM updated successfully');\n"
         << "    })\n"
         << "    .catch(function(e) { \n"
         << "      console.error('refreshStatus: fetch error:', e);\n"
         << "      console.error('refreshStatus: error details:', e.message, e.stack);\n"
         << "    });\n"
         << "}\n"
         << "console.log('refreshStatus function defined successfully');\n"
         << "\n"
         << "console.log('About to define refreshDevices function...');\n"
         << "function refreshDevices() {\n"
         << "  console.log('refreshDevices() called - starting device status fetch');\n"
         << "  console.log('About to fetch /device_status_json...');\n"
         << "  var deviceFetchPromise = fetch('/device_status_json');\n"
         << "  console.log('device fetch() called, promise object:', deviceFetchPromise);\n"
         << "  deviceFetchPromise\n"
         << "    .then(function(response) {\n"
         << "      console.log('Device status response received:', response.status, response.statusText);\n"
         << "      console.log('Response headers:', response.headers);\n"
         << "      console.log('Response URL:', response.url);\n"
         << "      if (!response.ok) {\n"
         << "        console.error('Response not OK, throwing error');\n"
         << "        throw new Error('HTTP ' + response.status);\n"
         << "      }\n"
         << "      console.log('Response OK, parsing JSON...');\n"
         << "      return response.json();\n"
         << "    })\n"
         << "    .then(function(data) {\n"
         << "      console.log('Device JSON data received:', JSON.stringify(data));\n"
         << "      if (!data.devices || !Array.isArray(data.devices)) {\n"
         << "        console.error('Invalid device data format:', data);\n"
         << "        throw new Error('Invalid device data format');\n"
         << "      }\n"
         << "      var devices = data.devices;\n"
         << "      console.log('Processing', devices.length, 'devices');\n"
         << "      var tbody = document.querySelector('#device-table tbody');\n"
         << "      if (!tbody) {\n"
         << "        console.error('Could not find tbody element');\n"
         << "        return;\n"
         << "      }\n"
         << "      console.log('Found tbody element, clearing content');\n"
         << "      tbody.innerHTML = '';\n"
         << "      for (var i = 0; i < devices.length; i++) {\n"
         << "        var device = devices[i];\n"
         << "        console.log('Adding device:', device.name, device.status);\n"
         << "        var row = tbody.insertRow();\n"
         << "        var nameCell = row.insertCell(0);\n"
         << "        var statusCell = row.insertCell(1);\n"
         << "        nameCell.textContent = device.name;\n"
         << "        statusCell.textContent = device.status;\n"
         << "        var statusClass = 'ok';\n"
         << "        if (device.status === 'fault') statusClass = 'fault';\n"
         << "        else if (device.status === 'operational') statusClass = 'operational';\n"
         << "        else if (device.status === 'degraded') statusClass = 'degraded';\n"
         << "        else if (device.status === 'active') statusClass = 'active';\n"
         << "        statusCell.className = statusClass;\n"
         << "      }\n"
         << "      if (data.timestamp) {\n"
         << "        console.log('Updating timestamp to:', data.timestamp);\n"
         << "        document.getElementById('last-updated').innerText = data.timestamp;\n"
         << "      }\n"
         << "      console.log('Table update completed successfully');\n"
         << "    })\n"
         << "    .catch(function(error) {\n"
         << "      console.error('Error fetching device status:', error);\n"
         << "      console.error('Error details:', error.message, error.stack);\n"
         << "      document.getElementById('last-updated').innerText = 'Error: ' + error.message;\n"
         << "    });\n"
         << "  console.log('About to fetch /check_status for system status...');\n"
         << "  var statusFetchPromise = fetch('/check_status');\n"
         << "  console.log('system status fetch() called, promise object:', statusFetchPromise);\n"
         << "  statusFetchPromise\n"
         << "    .then(function(response) {\n"
         << "      console.log('System status response received:', response.status, response.statusText);\n"
         << "      if (!response.ok) throw new Error('HTTP ' + response.status);\n"
         << "      return response.json();\n"
         << "    })\n"
         << "    .then(function(data) {\n"
         << "      console.log('System status data received:', JSON.stringify(data));\n"
         << "      if (data.status) {\n"
         << "        console.log('Updating system status to:', data.status);\n"
         << "        document.getElementById('status-value').innerText = data.status;\n"
         << "      }\n"
         << "    })\n"
         << "    .catch(function(error) {\n"
         << "      console.error('Error fetching system status:', error);\n"
         << "      console.error('System status error details:', error.message, error.stack);\n"
         << "    });\n"
         << "}\n"
         << "console.log('refreshDevices function defined successfully');\n"
         << "\n"
         << "console.log('About to define refreshAll function...');\n"
         << "function refreshAll() {\n"
         << "  console.log('refreshAll() called at:', new Date().toLocaleTimeString());\n"
         << "  refreshDevices();\n"
         << "}\n"
         << "console.log('refreshAll function defined successfully');\n"
         << "\n"
         << "console.log('About to define startRefreshTimer function...');\n"
         << "var refreshTimer; // Global timer variable\n"
         << "function startRefreshTimer() {\n"
         << "  console.log('startRefreshTimer() called');\n"
         << "  console.log('Starting refresh timer (10 second interval)...');\n"
         << "  // Clear existing timer if any\n"
         << "  if (refreshTimer) {\n"
         << "    clearInterval(refreshTimer);\n"
         << "    console.log('Cleared existing refresh timer');\n"
         << "  }\n"
         << "  refreshTimer = setInterval(function() {\n"
         << "    console.log('Timer triggered at:', new Date().toLocaleTimeString());\n"
         << "    refreshAll();\n"
         << "  }, 10000);\n"
         << "  console.log('Timer setup complete (10 second refresh)');\n"
         << "}\n"
         << "function resetRefreshTimer() {\n"
         << "  console.log('resetRefreshTimer() called - resetting 10s countdown');\n"
         << "  startRefreshTimer(); // This clears old timer and starts new one\n"
         << "}\n"
         << "console.log('startRefreshTimer function defined successfully');\n"
         << "\n"
         << "console.log('About to define DOMContentLoaded listener...');\n"
         << "document.addEventListener('DOMContentLoaded', function() {\n"
         << "  console.log('DOMContentLoaded event fired at:', new Date().toLocaleTimeString());\n"
         << "  console.log('Starting initial refresh in 500ms...');\n"
         << "  setTimeout(function() {\n"
         << "    console.log('Timeout fired, calling refreshAll()...');\n"
         << "    refreshAll();\n"
         << "  }, 500);\n"
         << "  startRefreshTimer();\n"
         << "});\n"
         << "console.log('DOMContentLoaded listener defined successfully');\n"
         << "\n"
         << "// Form submission functions\n"
         << "console.log('About to define form submission functions...');\n"
         << "function submitSystemUpdate() {\n"
         << "  console.log('submitSystemUpdate() called');\n"
         << "  var statusInput = document.getElementById('new-system-status');\n"
         << "  var newStatus = statusInput.value.trim();\n"
         << "  if (!newStatus) {\n"
         << "    alert('Please enter a system status');\n"
         << "    return;\n"
         << "  }\n"
         << "  console.log('Submitting system status update:', newStatus);\n"
         << "  var formData = new FormData();\n"
         << "  formData.append('system_status', newStatus);\n"
         << "  formData.append('source', 'webpage');\n"
         << "  console.log('About to fetch POST /update_system_web...');\n"
         << "  var updatePromise = fetch('/update_system_web', {\n"
         << "    method: 'POST',\n"
         << "    body: formData\n"
         << "  });\n"
         << "  console.log('system update fetch() called, promise object:', updatePromise);\n"
         << "  updatePromise\n"
         << "  .then(function(response) {\n"
         << "    console.log('System update response received:', response.status, response.statusText);\n"
         << "    if (!response.ok) throw new Error('HTTP ' + response.status);\n"
         << "    return response.text();\n"
         << "  })\n"
         << "  .then(function(data) {\n"
         << "    console.log('Update successful:', data);\n"
         << "    statusInput.value = '';\n"
         << "    // Reset the 10-second timer immediately (before showing alert)\n"
         << "    resetRefreshTimer();\n"
         << "    console.log('System update: 10s timer reset, next refresh in 10 seconds');\n"
         << "    // Show alert after timer is already started\n"
         << "    alert('System status updated successfully!');\n"
         << "  })\n"
         << "  .catch(function(error) {\n"
         << "    console.error('Update failed:', error);\n"
         << "    console.error('Update error details:', error.message, error.stack);\n"
         << "    alert('Failed to update system status: ' + error.message);\n"
         << "  });\n"
         << "}\n"
         << "\n"
         << "function submitDeviceUpdate() {\n"
         << "  console.log('submitDeviceUpdate() called');\n"
         << "  var deviceSelect = document.getElementById('device-select');\n"
         << "  var statusSelect = document.getElementById('status-select');\n"
         << "  var deviceName = deviceSelect.value;\n"
         << "  var newStatus = statusSelect.value;\n"
         << "  if (!deviceName || !newStatus) {\n"
         << "    alert('Please select both device and status');\n"
         << "    return;\n"
         << "  }\n"
         << "  console.log('Submitting device update:', deviceName, '->', newStatus);\n"
         << "  var formData = new FormData();\n"
         << "  formData.append('device_name', deviceName);\n"
         << "  formData.append('device_status', newStatus);\n"
         << "  formData.append('source', 'webpage');\n"
         << "  console.log('About to fetch POST /update_device_web...');\n"
         << "  var deviceUpdatePromise = fetch('/update_device_web', {\n"
         << "    method: 'POST',\n"
         << "    body: formData\n"
         << "  });\n"
         << "  console.log('device update fetch() called, promise object:', deviceUpdatePromise);\n"
         << "  deviceUpdatePromise\n"
         << "  .then(function(response) {\n"
         << "    console.log('Device update response received:', response.status, response.statusText);\n"
         << "    if (!response.ok) throw new Error('HTTP ' + response.status);\n"
         << "    return response.text();\n"
         << "  })\n"
         << "  .then(function(data) {\n"
         << "    console.log('Device update successful:', data);\n"
         << "    deviceSelect.selectedIndex = 0;\n"
         << "    statusSelect.selectedIndex = 0;\n"
         << "    // Reset the 10-second timer immediately (before showing alert)\n"
         << "    console.log('About to call resetRefreshTimer()...');\n"
         << "    resetRefreshTimer();\n"
         << "    console.log('Device update: 10s timer reset, next refresh in 10 seconds');\n"
         << "    // Show alert after timer is already started\n"
         << "    alert('Device status updated successfully!');\n"
         << "  })\n"
         << "  .catch(function(error) {\n"
         << "    console.error('Device update failed:', error);\n"
         << "    console.error('Device update error details:', error.message, error.stack);\n"
         << "    alert('Failed to update device status: ' + error.message);\n"
         << "  });\n"
         << "}\n"
         << "console.log('Form submission functions defined successfully');\n"
         << "\n"
         << "console.log('JavaScript loaded successfully');\n"
         << "</script>\n"
         << "</head>"
         << "<body><div class='container'>"
         << "<h1>COMM SYSTEM STATUS</h1>"
         << "<div class='status-summary'>"
         << "<h2>Current System Status</h2>"
         << "<div id='status-value' class='status-value'>" << system_status << "</div>"
         << "<div style='margin-top: 10px; font-size: 0.9em; color: #7f8c8d;'>"
         << "Last Updated: <span id='last-updated'>Loading...</span>"
         << "</div>"
         << "</div>"
         << ""
         << "<!-- System Status Update Form -->"
         << "<div class='update-form'>"
         << "<h3>Update System Status</h3>"
         << "<div class='form-row'>"
         << "<input type='text' id='new-system-status' placeholder='Enter new system status' style='width: 300px; padding: 8px; margin-right: 10px;'>"
         << "<button onclick='submitSystemUpdate()' style='padding: 8px 16px; background-color: #3498db; color: white; border: none; border-radius: 4px; cursor: pointer;'>Update System</button>"
         << "</div>"
         << "</div>"
         << ""
         << "<!-- Device Status Update Form -->"
         << "<div class='update-form'>"
         << "<h3>Update Device Status</h3>"
         << "<div class='form-row'>"
         << "<select id='device-select' style='padding: 8px; margin-right: 10px; width: 150px;'>"
         << "<option value=''>Select Device</option>";
    
    // Add device options based on current devices
    for (const auto& device : devices) {
        html << "<option value='" << device.name << "'>" << device.name << "</option>";
    }
    
    html << "</select>"
         << "<select id='status-select' style='padding: 8px; margin-right: 10px; width: 120px;'>"
         << "<option value=''>Select Status</option>"
         << "<option value='ok'>OK</option>"
         << "<option value='operational'>Operational</option>"
         << "<option value='active'>Active</option>"
         << "<option value='degraded'>Degraded</option>"
         << "<option value='fault'>Fault</option>"
         << "<option value='offline'>Offline</option>"
         << "</select>"
         << "<button onclick='submitDeviceUpdate()' style='padding: 8px 16px; background-color: #27ae60; color: white; border: none; border-radius: 4px; cursor: pointer;'>Update Device</button>"
         << "</div>"
         << "</div>"
         << ""
         << "<table id='device-table'>"
         << "<thead>"
         << "<tr><th>Device</th><th>Status</th></tr>"
         << "</thead>"
         << "<tbody>"
         << "</tbody></table></div></body></html>";

    std::string html_content = html.str();
    printf("[WEB] Generated HTML content length: %d bytes\n", (int)html_content.length());
    
    std::string response = "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: text/html\r\n";
    response += "Cache-Control: no-cache, no-store, must-revalidate\r\n";
    response += "Pragma: no-cache\r\n";
    response += "Expires: 0\r\n";
    response += "Content-Length: " + std::to_string(html_content.length()) + "\r\n\r\n" + html_content;
    
    return response;
}

// Generate response for status check "/check_status" (WEB only)
std::string handle_check_status_request(ThreadContext* ctx) {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    char timestamp[100];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&time_t));
    
    pthread_mutex_lock(&ctx->mutex);
    std::string status = ctx->system_status;
    pthread_mutex_unlock(&ctx->mutex);

    std::ostringstream json;
    json << "{\"status\":\"" << status << "\",\"timestamp\":\"" << timestamp << "\"}";
    std::string json_content = json.str();

    std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ";
    response += std::to_string(json_content.length()) + "\r\n\r\n" + json_content;
    return response;
}

// Generate JSON response for device status "/device_status_json" (WEB only)
std::string handle_device_status_json_request(ThreadContext* ctx) {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    char timestamp[100];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&time_t));
    
    pthread_mutex_lock(&ctx->mutex);
    std::vector<DeviceStatus> devices = ctx->device_statuses; // Copy for thread safety
    pthread_mutex_unlock(&ctx->mutex);

    printf("[WEB] Serving device status JSON: %d devices\n", (int)devices.size());

    std::ostringstream json;
    json << "{\"devices\":[";
    for (size_t i = 0; i < devices.size(); ++i) {
        if (i > 0) json << ",";
        json << "{\"name\":\"" << devices[i].name << "\",\"status\":\"" << devices[i].status << "\"}";
    }
    json << "],\"timestamp\":\"" << timestamp << "\"}";

    std::string json_content = json.str();
    printf("[WEB] JSON response: %s\n", json_content.c_str());
    
    std::string response = "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: application/json\r\n";
    response += "Cache-Control: no-cache, no-store, must-revalidate\r\n";
    response += "Pragma: no-cache\r\n";
    response += "Expires: 0\r\n";
    response += "Content-Length: " + std::to_string(json_content.length()) + "\r\n\r\n" + json_content;
    return response;
}

// Handle POST request to update system status from backend (BACKEND only)
std::string handle_update_system_request(ThreadContext* ctx, const std::string& body) {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    char timestamp[100];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&time_t));
    
    printf("[BACKEND] [%s] Raw POST body received: %s\n", timestamp, body.c_str());
    
    pthread_mutex_lock(&ctx->mutex);
    
    // Parse the form data
    std::istringstream body_stream(body);
    std::string pair;
    
    while (std::getline(body_stream, pair, '&')) {
        size_t eq_pos = pair.find('=');
        if (eq_pos != std::string::npos) {
            std::string key = url_decode(pair.substr(0, eq_pos));
            std::string value = url_decode(pair.substr(eq_pos + 1));
            
            printf("[BACKEND] [%s] Decoded key-value: '%s' = '%s'\n", timestamp, key.c_str(), value.c_str());
            
            if (key == "system_status") {
                ctx->system_status = value;
                printf("[BACKEND] [%s] System status updated: %s\n", timestamp, value.c_str());
            } else {
                // Update device status
                bool found = false;
                for (auto& device : ctx->device_statuses) {
                    if (device.name == key) {
                        if (device.status != value) {
                            printf("[BACKEND] [%s] Device '%s' status changed: %s -> %s\n", 
                                   timestamp, key.c_str(), device.status.c_str(), value.c_str());
                            device.status = value;
                        }
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    // Add new device if not found
                    ctx->device_statuses.push_back({key, value});
                    printf("[BACKEND] [%s] New device added: %s = %s\n", timestamp, key.c_str(), value.c_str());
                }
            }
        }
    }
    
    pthread_mutex_unlock(&ctx->mutex);
    return "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nOK";
}

// Handle POST request to update variables from backend (BACKEND only)
std::string handle_update_var_request(ThreadContext* ctx, const std::string& body) {
    size_t pos = body.find("name=");
    size_t pos2 = body.find("&value=");
    if (pos != std::string::npos && pos2 != std::string::npos) {
        std::string name = url_decode(body.substr(pos+5, pos2-pos-5));
        std::string value = url_decode(body.substr(pos2+7));

        pthread_mutex_lock(&ctx->mutex);
        if (ctx->app_vars.find(name) != ctx->app_vars.end()) {
            ctx->app_vars[name] = value;
            ctx->system_status = "Updated: " + name + "=" + value;
        }
        pthread_mutex_unlock(&ctx->mutex);
    }
    return "HTTP/1.1 303 See Other\r\nLocation: /\r\n\r\n";
}

// Forward system status update to backend monitor
void notify_backend_monitor(const std::string& system_status, const std::vector<DeviceStatus>& devices) {
    printf("[WEB] notify_backend_monitor called with system_status: '%s'\n", system_status.c_str());
    
    // Try to connect to backend monitor (assuming it listens on port 54321 for notifications)
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("[WEB] Failed to create socket for backend notification: %s\n", strerror(errno));
        return;
    }
    
    sockaddr_in backend_addr = {0};
    backend_addr.sin_family = AF_INET;
    backend_addr.sin_port = htons(54321);  // Backend notification port
    inet_pton(AF_INET, "127.0.0.1", &backend_addr.sin_addr);
    
    printf("[WEB] Attempting to connect to backend monitor at 127.0.0.1:54321\n");
    
    // Set a short timeout for connection attempt
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    if (connect(sock, (sockaddr*)&backend_addr, sizeof(backend_addr)) < 0) {
        printf("[WEB] Backend monitor not reachable (may not be running): %s\n", strerror(errno));
        close(sock);
        return;
    }
    
    printf("[WEB] Successfully connected to backend monitor\n");
    printf("[WEB] Successfully connected to backend monitor\n");
    
    // Send status update notification in simple format
    std::ostringstream notification;
    notification << "SYSTEM_STATUS_UPDATE:" << system_status << "\n";
    for (const auto& device : devices) {
        notification << "DEVICE:" << device.name << "=" << device.status << "\n";
    }
    notification << "END\n";
    
    std::string msg = notification.str();
    printf("[WEB] Sending notification message:\n%s", msg.c_str());
    
    ssize_t sent = send(sock, msg.c_str(), msg.length(), 0);
    if (sent < 0) {
        printf("[WEB] Failed to send notification: %s\n", strerror(errno));
    } else {
        printf("[WEB] Sent status update notification to backend monitor (%d bytes)\n", (int)sent);
    }
    
    close(sock);
}

// Handle POST request to update system status from webpage (WEB only)
std::string handle_update_system_web_request(ThreadContext* ctx, const std::string& body) {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    char timestamp[100];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&time_t));
    
    printf("[WEB] [%s] Raw POST body received: %s\n", timestamp, body.c_str());
    
    pthread_mutex_lock(&ctx->mutex);
    
    // Simplified multipart form parsing approach
    std::string system_status_value;
    
    // Look for the system_status field directly in the body
    size_t status_field_pos = body.find("name=\"system_status\"");
    if (status_field_pos != std::string::npos) {
        printf("[WEB] [%s] Found system_status field at position %zu\n", timestamp, status_field_pos);
        
        // Find the double newline after the Content-Disposition header
        size_t value_start = body.find("\n\n", status_field_pos);
        if (value_start == std::string::npos) {
            // Try with \r\n\r\n
            value_start = body.find("\r\n\r\n", status_field_pos);
            if (value_start != std::string::npos) {
                value_start += 4; // Skip \r\n\r\n
            }
        } else {
            value_start += 2; // Skip \n\n
        }
        
        if (value_start != std::string::npos) {
            printf("[WEB] [%s] Found value start at position %zu\n", timestamp, value_start);
            
            // Find the next boundary to determine where the value ends
            size_t value_end = body.find("------WebKit", value_start);
            if (value_end != std::string::npos) {
                system_status_value = body.substr(value_start, value_end - value_start);
                
                // Trim whitespace and newlines
                while (!system_status_value.empty() && 
                       (system_status_value.back() == '\n' || system_status_value.back() == '\r' || system_status_value.back() == ' ')) {
                    system_status_value.pop_back();
                }
                while (!system_status_value.empty() && 
                       (system_status_value.front() == '\n' || system_status_value.front() == '\r' || system_status_value.front() == ' ')) {
                    system_status_value.erase(0, 1);
                }
                
                printf("[WEB] [%s] Extracted system_status value: '%s'\n", timestamp, system_status_value.c_str());
            } else {
                printf("[WEB] [%s] Could not find value end boundary\n", timestamp);
            }
        } else {
            printf("[WEB] [%s] Could not find value start after Content-Disposition\n", timestamp);
        }
    } else {
        printf("[WEB] [%s] Could not find system_status field in body\n", timestamp);
    }
    
    // Update system status if provided
    if (!system_status_value.empty()) {
        ctx->system_status = system_status_value;
        printf("[WEB] [%s] System status updated: %s\n", timestamp, system_status_value.c_str());
        
        // Notify backend monitor of the status change
        std::vector<DeviceStatus> devices_copy = ctx->device_statuses; // Copy for thread safety
        pthread_mutex_unlock(&ctx->mutex);
        
        // Send notification to backend monitor (outside mutex to avoid blocking)
        notify_backend_monitor(system_status_value, devices_copy);
        
        printf("[WEB] [%s] System status update successful - client will reset 10s refresh timer\n", timestamp);
        return "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 7\r\n\r\nSuccess";
    } else {
        printf("[WEB] [%s] No system_status value found, not updating\n", timestamp);
    }
    
    pthread_mutex_unlock(&ctx->mutex);
    return "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 7\r\n\r\nSuccess";
}

// Handle POST request to update device status from webpage (WEB only)
std::string handle_update_device_web_request(ThreadContext* ctx, const std::string& body) {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    char timestamp[100];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&time_t));
    
    printf("[WEB] [%s] Raw POST body received: %s\n", timestamp, body.c_str());
    
    pthread_mutex_lock(&ctx->mutex);
    
    // Simplified multipart form parsing for device updates
    std::string device_name;
    std::string device_status;
    
    // Look for the device_name field
    size_t name_field_pos = body.find("name=\"device_name\"");
    if (name_field_pos != std::string::npos) {
        printf("[WEB] [%s] Found device_name field at position %zu\n", timestamp, name_field_pos);
        
        // Find the double newline after the Content-Disposition header
        size_t name_value_start = body.find("\n\n", name_field_pos);
        if (name_value_start == std::string::npos) {
            name_value_start = body.find("\r\n\r\n", name_field_pos);
            if (name_value_start != std::string::npos) {
                name_value_start += 4;
            }
        } else {
            name_value_start += 2;
        }
        
        if (name_value_start != std::string::npos) {
            size_t name_value_end = body.find("------WebKit", name_value_start);
            if (name_value_end != std::string::npos) {
                device_name = body.substr(name_value_start, name_value_end - name_value_start);
                
                // Trim whitespace and newlines
                while (!device_name.empty() && 
                       (device_name.back() == '\n' || device_name.back() == '\r' || device_name.back() == ' ')) {
                    device_name.pop_back();
                }
                while (!device_name.empty() && 
                       (device_name.front() == '\n' || device_name.front() == '\r' || device_name.front() == ' ')) {
                    device_name.erase(0, 1);
                }
                
                printf("[WEB] [%s] Extracted device_name: '%s'\n", timestamp, device_name.c_str());
            }
        }
    }
    
    // Look for the device_status field
    size_t status_field_pos = body.find("name=\"device_status\"");
    if (status_field_pos != std::string::npos) {
        printf("[WEB] [%s] Found device_status field at position %zu\n", timestamp, status_field_pos);
        
        // Find the double newline after the Content-Disposition header
        size_t status_value_start = body.find("\n\n", status_field_pos);
        if (status_value_start == std::string::npos) {
            status_value_start = body.find("\r\n\r\n", status_field_pos);
            if (status_value_start != std::string::npos) {
                status_value_start += 4;
            }
        } else {
            status_value_start += 2;
        }
        
        if (status_value_start != std::string::npos) {
            size_t status_value_end = body.find("------WebKit", status_value_start);
            if (status_value_end != std::string::npos) {
                device_status = body.substr(status_value_start, status_value_end - status_value_start);
                
                // Trim whitespace and newlines
                while (!device_status.empty() && 
                       (device_status.back() == '\n' || device_status.back() == '\r' || device_status.back() == ' ')) {
                    device_status.pop_back();
                }
                while (!device_status.empty() && 
                       (device_status.front() == '\n' || device_status.front() == '\r' || device_status.front() == ' ')) {
                    device_status.erase(0, 1);
                }
                
                printf("[WEB] [%s] Extracted device_status: '%s'\n", timestamp, device_status.c_str());
            }
        }
    }
    
    // Update device status if both name and status provided
    if (!device_name.empty() && !device_status.empty()) {
        bool found = false;
        for (auto& device : ctx->device_statuses) {
            if (device.name == device_name) {
                if (device.status != device_status) {
                    printf("[WEB] [%s] Device '%s' status changed: %s -> %s\n", 
                           timestamp, device_name.c_str(), device.status.c_str(), device_status.c_str());
                    device.status = device_status;
                }
                found = true;
                break;
            }
        }
        if (!found) {
            // Add new device if not found
            ctx->device_statuses.push_back({device_name, device_status});
            printf("[WEB] [%s] New device added: %s = %s\n", timestamp, device_name.c_str(), device_status.c_str());
        }
        
        // Get current system status and device list for notification
        std::string current_system_status = ctx->system_status;
        std::vector<DeviceStatus> devices_copy = ctx->device_statuses; // Copy for thread safety
        pthread_mutex_unlock(&ctx->mutex);
        
        // Send notification to backend monitor (outside mutex to avoid blocking)
        printf("[WEB] [%s] Sending device update notification to backend monitor\n", timestamp);
        notify_backend_monitor(current_system_status, devices_copy);
        
        printf("[WEB] [%s] Device status update successful - client will reset 10s refresh timer\n", timestamp);
        return "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 7\r\n\r\nSuccess";
    } else {
        printf("[WEB] [%s] Missing device_name or device_status, not updating\n", timestamp);
        pthread_mutex_unlock(&ctx->mutex);
        return "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nContent-Length: 23\r\n\r\nMissing required fields";
    }
}

// Route HTTP requests based on server type
std::string route_request(const HttpRequest& request, ThreadContext* ctx, int connection_id) {
    const char* server_type_str = (request.server_type == BACKEND_SERVER) ? "BACKEND" : "WEB";
    
    printf("[%s] connection %d processing %s %s\n", 
           server_type_str, connection_id, request.method.c_str(), request.path.c_str());

    if (request.server_type == BACKEND_SERVER) {
        // Backend API endpoints - only allow specific operations
        if (request.path == "/update_system" && request.method == "POST") {
            return handle_update_system_request(ctx, request.body);
        } else if (request.path == "/update_var" && request.method == "POST") {
            return handle_update_var_request(ctx, request.body);
        } else {
            printf("[BACKEND] connection %d: 404 Not Found for %s %s\n", 
                   connection_id, request.method.c_str(), request.path.c_str());
            return "HTTP/1.1 404 Not Found\r\n\r\nBackend API endpoint not found";
        }
    } else {
        // Web interface endpoints
        if (request.path == "/") {
            return handle_root_request(ctx);
        } else if (request.path == "/check_status") {
            return handle_check_status_request(ctx);
        } else if (request.path == "/device_status_json") {
            return handle_device_status_json_request(ctx);
        } else if (request.path == "/update_system_web" && request.method == "POST") {
            return handle_update_system_web_request(ctx, request.body);
        } else if (request.path == "/update_device_web" && request.method == "POST") {
            return handle_update_device_web_request(ctx, request.body);
        } else {
            printf("[WEB] connection %d: 404 Not Found for %s %s\n", 
                   connection_id, request.method.c_str(), request.path.c_str());
            return "HTTP/1.1 404 Not Found\r\n\r\nWeb endpoint not found";
        }
    }
}

// Handle client requests
void* handle_client(void* arg) {
    ClientThreadArgs* args = static_cast<ClientThreadArgs*>(arg);
    ThreadContext* ctx = args->ctx;
    int client_fd = args->client_fd;
    int connection_id = args->connection_id;
    ServerType server_type = args->server_type;
    delete args;

    const char* server_type_str = (server_type == BACKEND_SERVER) ? "BACKEND" : "WEB";

    // Set receive timeout (5 seconds)
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (true) {
        char buffer[4096];
        ssize_t bytes = recv(client_fd, buffer, sizeof(buffer)-1, 0);

        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                printf("[%s] Connection %d: Receive timeout\n", server_type_str, connection_id);
            } else {
                perror("recv failed");
            }
            break;
        } else if (bytes == 0) {
            printf("[%s] Connection %d: Client disconnected\n", server_type_str, connection_id);
            break;
        }

        buffer[bytes] = '\0';
        printf("[%s] Request recv connection %d: (%d bytes)\n", server_type_str, connection_id, (int)bytes);

        // Parse HTTP request
        HttpRequest request = parse_http_request(buffer, bytes, client_fd, server_type);
        printf("[%s] Connection %d: %s, keep_alive=%s\n", 
               server_type_str, connection_id, request.version.c_str(), request.keep_alive ? "true" : "false");

        // Route request and generate response
        std::string response = route_request(request, ctx, connection_id);
        
        // Send response
        send(client_fd, response.c_str(), response.length(), 0);
        printf("[%s] Sent response for connection %d\n", server_type_str, connection_id);

        // Break loop if not keep-alive
        if (!request.keep_alive) {
            printf("[%s] Closing connection %d (keep-alive: false)\n", server_type_str, connection_id);
            break;
        }

        printf("[%s] Keeping connection %d alive\n", server_type_str, connection_id);
    }

    // Update active connection count
    pthread_mutex_lock(&ctx->conn_mutex);
    if (server_type == BACKEND_SERVER) {
        ctx->active_backend_connections--;
        printf("[BACKEND] close connection %d (remaining backend connections: %d)\n",
               connection_id, ctx->active_backend_connections);
    } else {
        ctx->active_web_connections--;
        printf("[WEB] close connection %d (remaining web connections: %d)\n",
               connection_id, ctx->active_web_connections);
    }
    pthread_mutex_unlock(&ctx->conn_mutex);

    close(client_fd);
    return nullptr;
}

// Initialize server socket
int create_server_socket(int port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return -1;
    }

    // Set SO_REUSEADDR
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr))) {
        perror("bind");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 10)) {
        perror("listen");
        close(server_fd);
        return -1;
    }

    return server_fd;
}

// Initialize thread context with default values
void initialize_context(ThreadContext* context) {
    context->system_status = "Operational";
    context->app_vars["mode"] = "normal";
    context->app_vars["speed"] = "50";
    context->active_backend_connections = 0;
    context->active_web_connections = 0;

    // Initialize device statuses
    context->device_statuses.push_back({"Device1", "ok"});
    context->device_statuses.push_back({"Device2", "fault"});
    context->device_statuses.push_back({"Device3", "ok"});
    context->device_statuses.push_back({"Network Controller", "operational"});
    context->device_statuses.push_back({"Storage Unit", "degraded"});
    context->device_statuses.push_back({"Comm Link", "active"});

    pthread_mutex_init(&context->mutex, nullptr);
    pthread_mutex_init(&context->conn_mutex, nullptr);
}

int main() {
    printf("Dual-port web server starting...\n");
    
    const int BACKEND_PORT = 12345;  // Backend API port
    const int WEB_PORT = 8080;       // Web interface port

    // Initialize context
    ThreadContext context{};
    initialize_context(&context);

    // Create server sockets
    int backend_server_fd = create_server_socket(BACKEND_PORT);
    if (backend_server_fd < 0) {
        printf("Failed to create backend server socket\n");
        return 1;
    }
    printf("Backend API listening on port %d\n", BACKEND_PORT);

    int web_server_fd = create_server_socket(WEB_PORT);
    if (web_server_fd < 0) {
        printf("Failed to create web server socket\n");
        close(backend_server_fd);
        return 1;
    }
    printf("Web interface listening on port %d\n", WEB_PORT);

    int connection_counter = 0;
    int accept_counter = 0;
    
    // Main server loop using select()
    while (true) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(backend_server_fd, &read_fds);
        FD_SET(web_server_fd, &read_fds);
        
        int max_fd = std::max(backend_server_fd, web_server_fd);
        
        // Wait for activity on either server socket
        int activity = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        
        if (activity < 0) {
            perror("select");
            break;
        }
        
        // Check for new backend connections
        if (FD_ISSET(backend_server_fd, &read_fds)) {
            sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(backend_server_fd, (sockaddr*)&client_addr, &client_len);
            accept_counter++;
            
            if (client_fd < 0) {
                perror("backend accept");
                continue;
            }
            connection_counter++;

            // Update backend connection count
            pthread_mutex_lock(&context.conn_mutex);
            context.active_backend_connections++;
            int current_backend_connections = context.active_backend_connections;
            pthread_mutex_unlock(&context.conn_mutex);

            printf("BACKEND accepted connection: accept %d, connection %d (active backend connections: %d)\n",
                   accept_counter, connection_counter, current_backend_connections);

            // Create thread arguments for backend client
            ClientThreadArgs* args = new ClientThreadArgs();
            args->ctx = &context;
            args->client_fd = client_fd;
            args->connection_id = connection_counter;
            args->server_type = BACKEND_SERVER;

            // Create thread to handle backend client
            pthread_t thread;
            if (pthread_create(&thread, nullptr, handle_client, args)) {
                perror("pthread_create for backend");

                // Roll back connection count on error
                pthread_mutex_lock(&context.conn_mutex);
                context.active_backend_connections--;
                pthread_mutex_unlock(&context.conn_mutex);

                close(client_fd);
                delete args;
            } else {
                pthread_detach(thread);
            }
        }
        
        // Check for new web connections
        if (FD_ISSET(web_server_fd, &read_fds)) {
            sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(web_server_fd, (sockaddr*)&client_addr, &client_len);
            accept_counter++;
            
            if (client_fd < 0) {
                perror("web accept");
                continue;
            }
            connection_counter++;

            // Update web connection count
            pthread_mutex_lock(&context.conn_mutex);
            context.active_web_connections++;
            int current_web_connections = context.active_web_connections;
            pthread_mutex_unlock(&context.conn_mutex);

            printf("WEB accepted connection: accept %d, connection %d (active web connections: %d)\n",
                   accept_counter, connection_counter, current_web_connections);

            // Create thread arguments for web client
            ClientThreadArgs* args = new ClientThreadArgs();
            args->ctx = &context;
            args->client_fd = client_fd;
            args->connection_id = connection_counter;
            args->server_type = WEB_SERVER;

            // Create thread to handle web client
            pthread_t thread;
            if (pthread_create(&thread, nullptr, handle_client, args)) {
                perror("pthread_create for web");

                // Roll back connection count on error
                pthread_mutex_lock(&context.conn_mutex);
                context.active_web_connections--;
                pthread_mutex_unlock(&context.conn_mutex);

                close(client_fd);
                delete args;
            } else {
                pthread_detach(thread);
            }
        }
    }

    // Cleanup
    pthread_mutex_destroy(&context.mutex);
    pthread_mutex_destroy(&context.conn_mutex);
    close(backend_server_fd);
    close(web_server_fd);
    return 0;
}
