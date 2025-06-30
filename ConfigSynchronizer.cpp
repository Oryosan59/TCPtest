// ConfigSynchronizer.cpp - 修正版
//
// 目的:
// 1. config.ini ファイルを読み込む
// 2. TCPクライアントとして、現在の設定をWPFアプリケーションに送信する
// 3. TCPサーバーとして、WPFアプリケーションからの設定変更を待ち受け、動的に反映する
//
// 依存ライブラリ:
// - inih: https://github.com/benhoyt/inih から "ini.h" をダウンロードして同じディレクトリに配置してください
//
// コンパイル方法 (g++ の場合):
// g++ ConfigSynchronizer.cpp ini.c -o ConfigSynchronizer.exe -lws2_32 -std=c++11

#include <iostream>
#include <string>
#include <map>
#include <thread>
#include <mutex>
#include <vector>
#include <sstream>
#include <atomic>
#include <chrono>
#include <fstream>

// Winsock2を使用するために必要
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define CLOSE_SOCKET closesocket
#define SOCKET_ERROR_CODE WSAGetLastError()
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define CLOSE_SOCKET close
#define SOCKET_ERROR_CODE errno
#endif

// inihライブラリのヘッダ
#include "ini.h"

// グローバル変数
std::map<std::string, std::map<std::string, std::string>> g_config_data;
std::mutex g_config_mutex;
std::atomic<bool> g_shutdown_flag{false};

class ConfigSynchronizer {
private:
    std::string config_file_path;
    std::thread receiver_thread;
    SOCKET listen_socket = INVALID_SOCKET;

public:
    ConfigSynchronizer(const std::string& config_path) : config_file_path(config_path) {
#ifdef _WIN32
        // Winsockの初期化
        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) {
            throw std::runtime_error("WSAStartup failed: " + std::to_string(result));
        }
#endif
    }

    ~ConfigSynchronizer() {
        shutdown();
#ifdef _WIN32
        WSACleanup();
#endif
    }

    void shutdown() {
        g_shutdown_flag = true;
        
        // リスニングソケットを閉じる
        if (listen_socket != INVALID_SOCKET) {
            CLOSE_SOCKET(listen_socket);
            listen_socket = INVALID_SOCKET;
        }
        
        // 受信スレッドが終了するまで待機
        if (receiver_thread.joinable()) {
            receiver_thread.join();
        }
    }

    bool load_config() {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        g_config_data.clear();
        
        if (ini_parse(config_file_path.c_str(), ini_parse_handler, &g_config_data) < 0) {
            std::cerr << "エラー: '" << config_file_path << "' を読み込めません。\n";
            return false;
        }
        
        std::cout << "設定ファイルを " << config_file_path << " から読み込みました。\n";
        return true;
    }

    bool save_config() {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        std::ofstream file(config_file_path);
        if (!file.is_open()) {
            std::cerr << "エラー: '" << config_file_path << "' を書き込み用に開けません。\n";
            return false;
        }

        file << "# Navigator C++制御アプリケーションの設定ファイル\n";
        file << "# '#'または';'で始まる行はコメント行として扱われます\n";
        file << "# セクションは [セクション名] で定義され、キー=値 で設定します\n\n";

        for (const auto& section_pair : g_config_data) {
            file << "[" << section_pair.first << "]\n";
            for (const auto& key_value_pair : section_pair.second) {
                file << key_value_pair.first << "=" << key_value_pair.second << "\n";
            }
            file << "\n";
        }
        
        file.close();
        std::cout << "設定ファイルを " << config_file_path << " に保存しました。\n";
        return true;
    }

    std::string get_config_value(const std::string& section, const std::string& key, const std::string& default_value = "") {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        auto section_it = g_config_data.find(section);
        if (section_it != g_config_data.end()) {
            auto key_it = section_it->second.find(key);
            if (key_it != section_it->second.end()) {
                return key_it->second;
            }
        }
        return default_value;
    }

    void set_config_value(const std::string& section, const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        g_config_data[section][key] = value;
        std::cout << "設定更新: [" << section << "] " << key << " = " << value << std::endl;
    }

    std::string serialize_config() {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        std::stringstream ss;
        
        // メッセージ長を最初に送信するためのヘッダーを追加
        std::stringstream content;
        for (const auto& section_pair : g_config_data) {
            for (const auto& key_value_pair : section_pair.second) {
                content << "[" << section_pair.first << "]"
                       << key_value_pair.first << "=" << key_value_pair.second << "\n";
            }
        }
        
        std::string content_str = content.str();
        ss << content_str.length() << "\n" << content_str;
        return ss.str();
    }

    void update_config_from_string(const std::string& data) {
        std::stringstream ss(data);
        std::string line;
        bool config_changed = false;

        while (std::getline(ss, line)) {
            if (line.empty() || line[0] != '[') continue;

            size_t section_end = line.find(']');
            size_t equals_pos = line.find('=', section_end);

            if (section_end != std::string::npos && equals_pos != std::string::npos) {
                std::string section = line.substr(1, section_end - 1);
                std::string key = line.substr(section_end + 1, equals_pos - (section_end + 1));
                std::string value = line.substr(equals_pos + 1);

                // 改行コードなど、末尾の空白文字を削除
                value.erase(value.find_last_not_of(" \n\r\t") + 1);

                // 現在の値と比較して変更があった場合のみ更新
                std::string current_value = get_config_value(section, key);
                if (current_value != value) {
                    set_config_value(section, key, value);
                    config_changed = true;
                }
            }
        }

        // 設定が変更された場合はファイルに保存
        if (config_changed) {
            save_config();
        }
    }

    bool send_config_to_wpf() {
        std::string host = get_config_value("CONFIG_SYNC", "WPF_HOST", "127.0.0.1");
        int port;
        try {
            port = std::stoi(get_config_value("CONFIG_SYNC", "WPF_RECV_PORT", "12347"));
        } catch (const std::exception& e) {
            std::cerr << "エラー: 無効なポート番号です。\n";
            return false;
        }

        SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == INVALID_SOCKET) {
            std::cerr << "エラー: 送信用ソケットを作成できませんでした。 Code: " << SOCKET_ERROR_CODE << std::endl;
            return false;
        }

        // 接続タイムアウトを設定
        struct timeval timeout;
        timeout.tv_sec = 5;  // 5秒タイムアウト
        timeout.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

        sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        
#ifdef _WIN32
        if (inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr) <= 0) {
#else
        if (inet_aton(host.c_str(), &server_addr.sin_addr) == 0) {
#endif
            std::cerr << "エラー: 無効なIPアドレスです: " << host << std::endl;
            CLOSE_SOCKET(sock);
            return false;
        }

        if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
            std::cerr << "エラー: WPFアプリケーション(" << host << ":" << port << ")に接続できませんでした。 Code: " << SOCKET_ERROR_CODE << std::endl;
            CLOSE_SOCKET(sock);
            return false;
        }

        std::cout << "WPFアプリケーションに接続しました。設定を送信します...\n";
        std::string config_str = serialize_config();
        
        size_t total_sent = 0;
        while (total_sent < config_str.length()) {
            int bytes_sent = send(sock, config_str.c_str() + total_sent, 
                                static_cast<int>(config_str.length() - total_sent), 0);
            if (bytes_sent == SOCKET_ERROR) {
                std::cerr << "エラー: データ送信に失敗しました。 Code: " << SOCKET_ERROR_CODE << std::endl;
                CLOSE_SOCKET(sock);
                return false;
            }
            total_sent += bytes_sent;
        }

        CLOSE_SOCKET(sock);
        std::cout << "設定を送信し、接続を閉じました。\n";
        return true;
    }

    bool start_config_receiver() {
        receiver_thread = std::thread(&ConfigSynchronizer::receive_config_updates, this);
        return true;
    }

private:
    static int ini_parse_handler(void* user, const char* section, const char* name, const char* value) {
        auto* pconfig = static_cast<std::map<std::string, std::map<std::string, std::string>>*>(user);
        (*pconfig)[section][name] = value;
        return 1;
    }

    void receive_config_updates() {
        int port;
        try {
            port = std::stoi(get_config_value("CONFIG_SYNC", "CPP_RECV_PORT", "12348"));
        } catch (const std::exception& e) {
            std::cerr << "エラー: 無効な受信ポート番号です。\n";
            return;
        }

        listen_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_socket == INVALID_SOCKET) {
            std::cerr << "エラー: 受信用ソケットを作成できませんでした。 Code: " << SOCKET_ERROR_CODE << std::endl;
            return;
        }

        // SO_REUSEADDRオプションを設定
        int opt = 1;
        if (setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt)) < 0) {
            std::cerr << "エラー: SO_REUSEADDRの設定に失敗しました。\n";
        }

        sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(port);

        if (bind(listen_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
            std::cerr << "エラー: ポート " << port << " にバインドできませんでした。 Code: " << SOCKET_ERROR_CODE << std::endl;
            CLOSE_SOCKET(listen_socket);
            listen_socket = INVALID_SOCKET;
            return;
        }

        if (listen(listen_socket, 5) == SOCKET_ERROR) {
            std::cerr << "エラー: listenに失敗しました。 Code: " << SOCKET_ERROR_CODE << std::endl;
            CLOSE_SOCKET(listen_socket);
            listen_socket = INVALID_SOCKET;
            return;
        }

        std::cout << "ポート " << port << " でWPFからの設定更新を待機しています...\n";

        while (!g_shutdown_flag) {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(listen_socket, &read_fds);

            struct timeval timeout;
            timeout.tv_sec = 1;
            timeout.tv_usec = 0;

            int activity = select(static_cast<int>(listen_socket + 1), &read_fds, NULL, NULL, &timeout);
            
            if (activity == SOCKET_ERROR) {
                if (!g_shutdown_flag) {
                    std::cerr << "エラー: selectに失敗しました。 Code: " << SOCKET_ERROR_CODE << std::endl;
                }
                break;
            }
            
            if (activity > 0 && FD_ISSET(listen_socket, &read_fds)) {
                SOCKET client_sock = accept(listen_socket, NULL, NULL);
                if (client_sock == INVALID_SOCKET) {
                    if (!g_shutdown_flag) {
                        std::cerr << "エラー: acceptに失敗しました。 Code: " << SOCKET_ERROR_CODE << std::endl;
                    }
                    continue;
                }

                handle_client_connection(client_sock);
                CLOSE_SOCKET(client_sock);
            }
        }

        if (listen_socket != INVALID_SOCKET) {
            CLOSE_SOCKET(listen_socket);
            listen_socket = INVALID_SOCKET;
        }
    }

    void handle_client_connection(SOCKET client_sock) {
        const int BUFFER_SIZE = 8192;
        std::vector<char> recv_buf(BUFFER_SIZE);
        std::string received_data;

        // まずメッセージ長を受信
        int bytes_received = recv(client_sock, recv_buf.data(), recv_buf.size() - 1, 0);
        if (bytes_received <= 0) {
            return;
        }

        recv_buf[bytes_received] = '\0';
        std::string header(recv_buf.data());
        
        size_t newline_pos = header.find('\n');
        if (newline_pos == std::string::npos) {
            // 古い形式（長さヘッダーなし）として処理
            received_data = header;
        } else {
            // 新しい形式（長さヘッダーあり）として処理
            try {
                size_t expected_length = std::stoull(header.substr(0, newline_pos));
                received_data = header.substr(newline_pos + 1);
                
                // 残りのデータを受信
                while (received_data.length() < expected_length) {
                    int remaining_bytes = recv(client_sock, recv_buf.data(), 
                                             std::min(recv_buf.size() - 1, expected_length - received_data.length()), 0);
                    if (remaining_bytes <= 0) break;
                    
                    recv_buf[remaining_bytes] = '\0';
                    received_data += recv_buf.data();
                }
            } catch (const std::exception& e) {
                std::cerr << "エラー: メッセージ長の解析に失敗しました。\n";
                return;
            }
        }

        if (!received_data.empty()) {
            std::cout << "\nWPFから設定データを受信しました。\n";
            update_config_from_string(received_data);
        }
    }
};

int main(int argc, char* argv[]) {
    try {
        // config.iniのパスを指定
        std::string config_path = "config.ini";
        if (argc > 1) {
            config_path = argv[1];
        }

        ConfigSynchronizer sync(config_path);

        // 初期設定をファイルから読み込む
        if (!sync.load_config()) {
            return 1;
        }

        // WPFからの設定更新を待ち受けるスレッドを開始
        if (!sync.start_config_receiver()) {
            std::cerr << "エラー: 受信スレッドの開始に失敗しました。\n";
            return 1;
        }

        // 少し待ってから、最初の設定をWPFに送信
        std::this_thread::sleep_for(std::chrono::seconds(1));
        sync.send_config_to_wpf();

        std::cout << "\nメインの処理を実行中...\n";
        std::cout << "コマンド:\n";
        std::cout << "  s - 設定をWPFに送信\n";
        std::cout << "  r - 設定ファイルを再読み込み\n";
        std::cout << "  q - 終了\n";

        // メインループ
        while (true) {
            std::string line;
            std::getline(std::cin, line);
            
            if (line == "q") {
                break;
            } else if (line == "s") {
                std::cout << "現在の設定をWPFに送信します。\n";
                sync.send_config_to_wpf();
            } else if (line == "r") {
                std::cout << "設定ファイルを再読み込みします。\n";
                sync.load_config();
                sync.send_config_to_wpf();
            } else if (line.empty()) {
                // Enterキーのみの場合は設定を再送信（後方互換性）
                std::cout << "現在の設定をWPFに再送信します。\n";
                sync.send_config_to_wpf();
            }
        }

        sync.shutdown();
        std::cout << "アプリケーションを終了します。\n";

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "致命的エラー: " << e.what() << std::endl;
        return 1;
    }
}