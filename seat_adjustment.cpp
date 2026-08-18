#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>
#include <iostream>

using ordered_json = nlohmann::ordered_json;  
#define BUFFER_SIZE 2048
#define NXP_PORT 44821
#define NXP_IP "192.168.1.102"
#define ANDROID_PORT 60001

void send_to_nxp(std::string json_str) {
    int sock;
    struct sockaddr_in server_addr;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed (NXP)");
        return;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(NXP_PORT);
    inet_pton(AF_INET, NXP_IP, &server_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection to NXP failed");
        close(sock);
        return;
    }

    send(sock, json_str.c_str(), json_str.size(), 0);
    close(sock);

    printf("Sent to NXP: %s\n", json_str.c_str());
}


void kill_process_on_port(int port) {
    std::ostringstream cmd;
    cmd << "ss -tulnp | grep :" << port << " | awk '{print $7}'";

    FILE* pipe = popen(cmd.str().c_str(), "r");
    if (!pipe) return;

    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        std::string pid_str(buffer);

        // Trim whitespace
        pid_str.erase(std::remove(pid_str.begin(), pid_str.end(), '\n'), pid_str.end());
        pid_str.erase(std::remove(pid_str.begin(), pid_str.end(), ' '), pid_str.end());
        if (pid_str.empty()) continue;

        int pid = 0;

        // Case 1: "users:(("prog",pid=1234,fd=3))"
        size_t pos = pid_str.find("pid=");
        if (pos != std::string::npos) {
            std::string sub = pid_str.substr(pos + 4);
            sub = sub.substr(0, sub.find(",")); // cut at next comma
            pid = atoi(sub.c_str());
        }
        // Case 2: "1234/prog"
        else if (isdigit(pid_str[0])) {
            pid = atoi(pid_str.c_str());
        }

        if (pid > 0) {
            std::cout << "⚠ Port " << port << " already in use by PID " << pid << ". Killing...\n";
            std::string ps_cmd = "ps -p " + std::to_string(pid) + " -o comm=";
            system(ps_cmd.c_str());
            std::string kill_cmd = "kill -9 " + std::to_string(pid);
            system(kill_cmd.c_str());
        }
    }
    pclose(pipe);
}
void adjust_seat(const std::string& name, ordered_json current, ordered_json target, int android_socket) {
    const std::vector<std::string> update_order = {"Back", "HPos", "VPos", "Headrest"};

    auto build_payload = [&](const std::string& status, const ordered_json& cur, const ordered_json& tgt) {
        ordered_json payload;
        payload["Status"] = status;
        payload["SeatType"] = name;

        ordered_json seat_data, target_data;
        for (const auto& key : update_order) {
            seat_data[key] = cur.value(key, 0);
            target_data[key] = tgt.value(key, 0);
        }
        payload["Seat"] = seat_data;
        payload["TargetSeat"] = target_data;
        return payload;
    };

    // ------------------ START ------------------
    std::string start_str = build_payload("Start", current, target).dump() + "\n";
    send(android_socket, start_str.c_str(), start_str.size(), 0);
    printf("Sent to Android: %s", start_str.c_str());
    std::thread([=]() {
        sleep(1);  // 1s delay
        send_to_nxp(start_str);
    }).detach();

  // ------------------ PROGRESS ------------------
for (const std::string& key : update_order) {
    if (!current.contains(key) || !target.contains(key)) continue;
    if (key == "Headrest") continue;  

    int current_val = current[key].get<int>();
    int target_val = target[key].get<int>();
    
    int step = 1; // Always increment/decrement by 1 now

    while (current_val != target_val) {
        if (current_val < target_val) {
            current_val += step;
            if (current_val > target_val) current_val = target_val;
        } else {
            current_val -= step;
            if (current_val < target_val) current_val = target_val;
        }
        current[key] = current_val;

        ordered_json progress_msg = build_payload("InProgress", current, target);
        std::string json_str = progress_msg.dump() + "\n";

        // Send immediately to Android
        send(android_socket, json_str.c_str(), json_str.size(), 0);
        printf("Sent to Android: %s", json_str.c_str());

        // Send to NXP with delay
        std::thread([=]() {
            sleep(1);  // 1s behind Android
            send_to_nxp(json_str);
        }).detach();

        usleep(50 * 1000);  // 50ms pacing for Android
    }
}


    // ------------------ END ------------------
    std::string end_str = build_payload("End", current, target).dump() + "\n";
    send(android_socket, end_str.c_str(), end_str.size(), 0);
    printf("Sent to Android: %s", end_str.c_str());
    std::thread([=]() {
        sleep(1);
        send_to_nxp(end_str);
    }).detach();
}

int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    char buffer[BUFFER_SIZE];
    int opt = 1;
    socklen_t addrlen = sizeof(address);
    
    kill_process_on_port(ANDROID_PORT);
    kill_process_on_port(NXP_PORT)
;
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(ANDROID_PORT);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 3) < 0) {
        perror("Listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Waiting for Android connection on port 60001...\n");

    while (1) {
        new_socket = accept(server_fd, (struct sockaddr*)&address, &addrlen);
        if (new_socket < 0) {
            perror("Accept failed");
            continue;
        }

        memset(buffer, 0, BUFFER_SIZE);
        ssize_t read_bytes = read(new_socket, buffer, BUFFER_SIZE - 1);
        if (read_bytes <= 0) {
            close(new_socket);
            continue;
        }

        buffer[read_bytes] = '\0';
        printf("Received: %s\n", buffer);

        try {
            auto received_json = ordered_json::parse(buffer);

            if (!received_json.contains("SeatType") ||
                !received_json.contains("CurrentSeat") ||
                !received_json.contains("TargetSeat")) {
                fprintf(stderr, "Invalid JSON format (missing keys)\n");
                close(new_socket);
                continue;
            }

            std::string name = received_json["SeatType"];
            ordered_json current = received_json["CurrentSeat"];
            ordered_json target = received_json["TargetSeat"];

            adjust_seat(name, current, target, new_socket);

        } catch (std::exception& e) {
            fprintf(stderr, "JSON Parse Error: %s\n", e.what());
        }

        close(new_socket);
    }

    close(server_fd);
    return 0;
}
