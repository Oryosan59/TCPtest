// ConfigSynchronizer.cpp - Raspberry Pi版
//
// 目的:
// 1. config.ini ファイルを読み込む
// 2. TCPクライアントとして、現在の設定をWPFアプリケーションに送信する
// 3. TCPサーバーとして、WPFアプリケーションからの設定変更を待ち受け、動的に反映する
//
// 依存ライブラリ:
// - libiniparser-dev: sudo apt install libiniparser-dev
//
// コンパイル方法:
// g++ -std=c++11 ConfigSynchronizer.cpp -o ConfigSynchronizer -liniparser -lpthread

#include <iostream>
#include <string>
#include <map>
#include <thread>
#include <mutex>
#include <vector>
#include <sstream>
#include <fstream>
#include <chrono>
#include <atomic>

// Linux用のソケットライブラリ
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>

// iniparserライブラリ（Raspberry Piで利用可能）
// libiniparser-devはヘッダをサブディレクトリにインストールするため、パスを修正
#include <iniparser/iniparser.h>
// グローバル変数: 設定データと、スレッドセーフなアクセスのためのミューテックス
std::map<std::string, std::map<std::string, std::string>> g_config_data;
std::mutex g_config_mutex;
std::atomic<bool> g_shutdown_flag{false};

/**
 * @brief iniファイルから設定を読み込む
 * @param filename config.iniのパス
 * @return 読み込みが成功した場合はtrue
 */
bool load_config(const std::string& filename) {
    dictionary* ini = iniparser_load(filename.c_str());
    if (ini == nullptr) {
        std::cerr << "エラー: '" << filename << "' を読み込めません。\n";
        return false;
    }

    std::lock_guard<std::mutex> lock(g_config_mutex);
    g_config_data.clear();

    // 辞書内のすべてのキーを反復処理する
    int n_keys = iniparser_getndict(ini);
    for (int i = 0; i < n_keys; i++) {
        const char* full_key = iniparser_getkey(ini, nullptr, i);
        if (full_key == nullptr) {
            continue;
        }

        // iniparserはキーを "section:key" の形式で返すため、これを分割する
        std::string full_key_str(full_key);
        size_t colon_pos = full_key_str.find(':');
        if (colon_pos == std::string::npos) {
            continue; // セクションがないキーは無視
        }

        std::string section = full_key_str.substr(0, colon_pos);
        std::string key = full_key_str.substr(colon_pos + 1);
        const char* value = iniparser_getstring(ini, full_key, "");
        g_config_data[section][key] = std::string(value);
    }

    iniparser_freedict(ini);
    std::cout << "設定ファイルを " << filename << " から読み込みました。\n";
    return true;
}

/**
 * @brief 設定値を安全に取得する
 * @param section セクション名
 * @param key キー名
 * @param default_value デフォルト値
 * @return 設定値またはデフォルト値
 */
std::string get_config_value(const std::string& section, const std::string& key, const std::string& default_value = "") {
    std::lock_guard<std::mutex> lock(g_config_mutex);
    auto section_it = g_config_data.find(section);
    if (section_it == g_config_data.end()) {
        return default_value;
    }
    auto key_it = section_it->second.find(key);
    if (key_it == section_it->second.end()) {
        return default_value;
    }
    return key_it->second;
}

/**
 * @brief 現在の設定データをWPFへ送信するための文字列形式に変換（シリアライズ）する
 * @return シリアライズされた設定文字列
 */
std::string serialize_config() {
    std::lock_guard<std::mutex> lock(g_config_mutex);
    std::stringstream ss;
    std::stringstream content_ss;
    for (const auto& section_pair : g_config_data) {
        for (const auto& key_value_pair : section_pair.second) {
            // フォーマット: [SECTION]KEY=VALUE\n
            content_ss << "[" << section_pair.first << "]"
               << key_value_pair.first << "=" << key_value_pair.second << "\n";
        }
    }
    // 確実なTCP通信のため、[メッセージ長]\n[メッセージ本体] という形式で送信する
    std::string content = content_ss.str();
    ss << content.length() << "\n" << content;
    return ss.str();
}

/**
 * @brief WPFから受信した文字列をパースして設定データを更新する
 * @param data 受信した文字列データ
 */
void update_config_from_string(const std::string& data) {
    std::stringstream ss(data);
    std::string line;

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

            std::lock_guard<std::mutex> lock(g_config_mutex);
            g_config_data[section][key] = value;
            std::cout << "設定更新: [" << section << "] " << key << " = " << value << std::endl;
        }
    }
}

/**
 * @brief WPFアプリケーションに現在の設定を送信する
 */
void send_config_to_wpf() {
    std::string host = get_config_value("CONFIG_SYNC", "WPF_HOST", "192.168.4.10");
    std::string port_str = get_config_value("CONFIG_SYNC", "WPF_RECV_PORT", "12347");
    
    int port;
    try {
        port = std::stoi(port_str);
    } catch (const std::exception& e) {
        std::cerr << "エラー: 不正なポート番号: " << port_str << std::endl;
        return;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "エラー: 送信用ソケットを作成できませんでした。" << strerror(errno) << std::endl;
        return;
    }

    // ソケットタイムアウトを設定
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr) <= 0) {
        std::cerr << "エラー: 不正なIPアドレス: " << host << std::endl;
        close(sock);
        return;
    }

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "エラー: WPFアプリケーション(" << host << ":" << port << ")に接続できませんでした。 " << strerror(errno) << std::endl;
        close(sock);
        return;
    }

    std::cout << "WPFアプリケーションに接続しました。設定を送信します...\n";
    std::string config_str = serialize_config();
    
    // 全てのデータが送信されるまでループする
    ssize_t total_sent = 0;
    const char* data_ptr = config_str.c_str();
    size_t data_len = config_str.length();

    while (total_sent < (ssize_t)data_len) {
        ssize_t bytes_sent = send(sock, data_ptr + total_sent, data_len - total_sent, 0);
        if (bytes_sent < 0) {
            std::cerr << "エラー: データ送信に失敗しました。 " << strerror(errno) << std::endl;
            close(sock);
            return;
        }
        total_sent += bytes_sent;
    }

    std::cout << "設定を送信しました（" << total_sent << " バイト）\n";
    close(sock);
    std::cout << "接続を閉じました。\n";
}

/**
 * @brief WPFからの設定更新を待ち受けるサーバーとして動作する (別スレッドで実行)
 */
void handle_client_connection(int client_sock); // プロトタイプ宣言

void receive_config_updates() {
    std::string port_str = get_config_value("CONFIG_SYNC", "CPP_RECV_PORT", "12348");
    
    int port;
    try {
        port = std::stoi(port_str);
    } catch (const std::exception& e) {
        std::cerr << "エラー: 不正なポート番号: " << port_str << std::endl;
        return;
    }

    int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        std::cerr << "エラー: 受信用ソケットを作成できませんでした。 " << strerror(errno) << std::endl;
        return;
    }

    // ソケットオプション設定（アドレス再利用）
    int opt = 1;
    if (setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "エラー: SO_REUSEADDRの設定に失敗しました。 " << strerror(errno) << std::endl;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(listen_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "エラー: ポート " << port << " にバインドできませんでした。 " << strerror(errno) << std::endl;
        close(listen_sock);
        return;
    }

    if (listen(listen_sock, 5) < 0) {
        std::cerr << "エラー: listenに失敗しました。 " << strerror(errno) << std::endl;
        close(listen_sock);
        return;
    }

    std::cout << "ポート " << port << " でWPFからの設定更新を待機しています...\n";

    while (!g_shutdown_flag.load()) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_sock, &readfds);
        
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int activity = select(listen_sock + 1, &readfds, nullptr, nullptr, &timeout);
        
        if (activity < 0) {
            if (errno != EINTR) {
                std::cerr << "エラー: selectに失敗しました。 " << strerror(errno) << std::endl;
            }
            break;
        }
        
        if (activity == 0) {
            // タイムアウト、継続
            continue;
        }
        
        if (FD_ISSET(listen_sock, &readfds)) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_sock = accept(listen_sock, (struct sockaddr*)&client_addr, &client_len);
            
            if (client_sock < 0) {
                std::cerr << "エラー: acceptに失敗しました。 " << strerror(errno) << std::endl;
                continue;
            }

            // 接続処理を別関数に委譲
            handle_client_connection(client_sock);
        }
    }

    close(listen_sock);
    std::cout << "設定更新受信スレッドを終了しました。\n";
}

/**
 * @brief クライアントからの接続を処理し、完全なメッセージを受信する
 * @param client_sock クライアントのソケットディスクリプタ
 */
void handle_client_connection(int client_sock) {
    // 1. ヘッダー（メッセージ長）を改行まで読み込む
    std::string header;
    char c;
    while (recv(client_sock, &c, 1, 0) > 0) {
        if (c == '\n') {
            break;
        }
        header += c;
    }

    if (header.empty()) {
        close(client_sock);
        return;
    }

    // 2. メッセージ長をパースし、その長さのデータを受信する
    try {
        size_t expected_length = std::stoull(header);
        std::string received_data;
        received_data.reserve(expected_length);
        
        std::vector<char> buffer(4096);
        size_t total_received = 0;

        while (total_received < expected_length) {
            size_t to_read = std::min(buffer.size(), expected_length - total_received);
            ssize_t bytes_received = recv(client_sock, buffer.data(), to_read, 0);
            if (bytes_received <= 0) {
                std::cerr << "エラー: データ受信中に接続が切れました。" << std::endl;
                close(client_sock);
                return;
            }
            received_data.append(buffer.data(), bytes_received);
            total_received += bytes_received;
        }
        
        std::cout << "\nWPFから設定データを受信しました（" << total_received << " バイト）\n";
        update_config_from_string(received_data);
    } catch (const std::exception& e) {
        std::cerr << "エラー: 不正なメッセージ長ヘッダー: " << header << std::endl;
    }
    close(client_sock);
}

/**
 * @brief 設定ファイルに現在の設定を保存する
 * @param filename 保存先ファイル名
 */
void save_config(const std::string& filename) {
    std::lock_guard<std::mutex> lock(g_config_mutex);
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "エラー: 設定ファイル " << filename << " を書き込み用に開けませんでした。\n";
        return;
    }

    for (const auto& section_pair : g_config_data) {
        file << "[" << section_pair.first << "]\n";
        for (const auto& key_value_pair : section_pair.second) {
            file << key_value_pair.first << "=" << key_value_pair.second << "\n";
        }
        file << "\n";
    }
    
    file.close();
    std::cout << "設定を " << filename << " に保存しました。\n";
}

/**
 * @brief 現在の設定を表示する
 */
void print_current_config() {
    std::lock_guard<std::mutex> lock(g_config_mutex);
    std::cout << "\n=== 現在の設定 ===\n";
    for (const auto& section_pair : g_config_data) {
        std::cout << "[" << section_pair.first << "]\n";
        for (const auto& key_value_pair : section_pair.second) {
            std::cout << "  " << key_value_pair.first << " = " << key_value_pair.second << "\n";
        }
        std::cout << "\n";
    }
    std::cout << "==================\n\n";
}

int main(int argc, char* argv[]) {
    // config.iniのパスを指定
    std::string config_path = "config.ini";
    if (argc > 1) {
        config_path = argv[1];
    }

    // 初期設定をファイルから読み込む
    if (!load_config(config_path)) {
        return 1;
    }

    // WPFからの設定更新を待ち受けるスレッドを開始
    std::thread receiver_thread(receive_config_updates);

    // 少し待ってから、最初の設定をWPFに送信
    std::this_thread::sleep_for(std::chrono::seconds(1));
    send_config_to_wpf();

    std::cout << "\nメインの処理を実行中...\n";
    std::cout << "コマンド:\n";
    std::cout << "  Enter: 現在設定を再送信\n";
    std::cout << "  s: 設定を表示\n";
    std::cout << "  w: 設定をファイルに保存\n";
    std::cout << "  q: 終了\n";

    // メインスレッドでは、他の処理を実行できる
    // ここではデモとして、ユーザー入力に応じて設定を再送信する
    while (true) {
        std::string line;
        std::getline(std::cin, line);
        
        if (line == "q") {
            break;
        } else if (line == "s") {
            print_current_config();
        } else if (line == "w") {
            save_config(config_path + ".backup");
        } else {
            std::cout << "現在の設定をWPFに再送信します。\n";
            send_config_to_wpf();
        }
    }

    // 終了処理
    std::cout << "終了処理中...\n";
    g_shutdown_flag.store(true);
    
    if (receiver_thread.joinable()) {
        receiver_thread.join();
    }

    std::cout << "プログラムを終了します。\n";
    return 0;
}