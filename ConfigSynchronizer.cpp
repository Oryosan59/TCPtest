// ConfigSynchronizer.cpp - Raspberry Pi版 (改良版)
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
#include <set>
#include <algorithm>

// Linux用のソケットライブラリ
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <signal.h>

// iniparserライブラリ（Raspberry Piで利用可能）
#include <iniparser/iniparser.h>

// グローバル変数: 設定データと、スレッドセーフなアクセスのためのミューテックス
std::map<std::string, std::map<std::string, std::string>> g_config_data;
std::mutex g_config_mutex;
std::atomic<bool> g_shutdown_flag{false};

// シグナルハンドラー用
void signal_handler(int signum) {
    std::cout << "\nシグナル " << signum << " を受信しました。終了処理を開始します...\n";
    g_shutdown_flag.store(true);
}

/**
 * @brief iniファイルから設定を読み込む (改良版)
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

    // セクション数を取得
    int n_sections = iniparser_getnsec(ini);
    
    for (int i = 0; i < n_sections; i++) {
        const char* section_name = iniparser_getsecname(ini, i);
        if (section_name == nullptr) {
            continue;
        }
        
        std::string section(section_name);
        
        // 全キーを取得するため、iniparserの内部構造を利用
        // より包括的なキーリストを定義
        std::vector<std::string> common_keys = {
            // CONFIG_SYNC section
            "WPF_HOST", "WPF_RECV_PORT", "CPP_RECV_PORT",
            // PWM section
            "PWM_MIN", "PWM_NEUTRAL", "PWM_NORMAL_MAX", "PWM_BOOST_MAX", "PWM_FREQUENCY",
            // JOYSTICK section
            "DEADZONE",
            // LED section
            "CHANNEL", "ON_VALUE", "OFF_VALUE",
            // THRUSTER_CONTROL section
            "SMOOTHING_FACTOR_HORIZONTAL", "SMOOTHING_FACTOR_VERTICAL",
            "KP_ROLL", "KP_YAW", "YAW_THRESHOLD_DPS", "YAW_GAIN",
            // NETWORK section
            "RECV_PORT", "SEND_PORT", "CLIENT_HOST", "CONNECTION_TIMEOUT_SECONDS",
            // APPLICATION section
            "SENSOR_SEND_INTERVAL", "LOOP_DELAY_US",
            // GSTREAMER_CAMERA sections
            "DEVICE", "PORT", "WIDTH", "HEIGHT", "FRAMERATE_NUM", "FRAMERATE_DEN",
            "IS_H264_NATIVE_SOURCE", "RTP_PAYLOAD_TYPE", "RTP_CONFIG_INTERVAL",
            "X264_BITRATE", "X264_TUNE", "X264_SPEED_PRESET"
        };
        
        for (const std::string& key : common_keys) {
            std::string full_key = section + ":" + key;
            const char* value = iniparser_getstring(ini, full_key.c_str(), nullptr);
            if (value != nullptr) {
                g_config_data[section][key] = std::string(value);
            }
        }
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
 * @brief 設定値を安全に設定する
 * @param section セクション名
 * @param key キー名
 * @param value 設定する値
 */
void set_config_value(const std::string& section, const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(g_config_mutex);
    g_config_data[section][key] = value;
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
    int updates_count = 0;

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

            // 値が変更された場合のみ更新ログを出力
            std::string old_value = get_config_value(section, key);
            if (old_value != value) {
                set_config_value(section, key, value);
                std::cout << "設定更新: [" << section << "] " << key << " = " << value;
                if (!old_value.empty()) {
                    std::cout << " (旧値: " << old_value << ")";
                }
                std::cout << std::endl;
                updates_count++;
            }
        }
    }
    
    if (updates_count > 0) {
        std::cout << "合計 " << updates_count << " 項目の設定を更新しました。\n";
    } else {
        std::cout << "設定に変更はありませんでした。\n";
    }
}

/**
 * @brief ソケットのノンブロッキングモードを設定する
 * @param sock ソケットディスクリプタ
 * @param non_blocking trueでノンブロッキング、falseでブロッキング
 * @return 成功時true
 */
bool set_socket_non_blocking(int sock, bool non_blocking) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1) {
        return false;
    }
    
    if (non_blocking) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    
    return fcntl(sock, F_SETFL, flags) != -1;
}

/**
 * @brief WPFアプリケーションに現在の設定を送信する (改良版)
 */
void send_config_to_wpf() {
    std::string host = get_config_value("CONFIG_SYNC", "WPF_HOST", "192.168.4.10");
    std::string port_str = get_config_value("CONFIG_SYNC", "WPF_RECV_PORT", "12347");
    
    int port;
    try {
        port = std::stoi(port_str);
        if (port <= 0 || port > 65535) {
            throw std::out_of_range("ポート番号が範囲外です");
        }
    } catch (const std::exception& e) {
        std::cerr << "エラー: 不正なポート番号: " << port_str << " (" << e.what() << ")" << std::endl;
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
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        std::cerr << "警告: 送信タイムアウトの設定に失敗しました。\n";
    }
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        std::cerr << "警告: 受信タイムアウトの設定に失敗しました。\n";
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr) <= 0) {
        std::cerr << "エラー: 不正なIPアドレス: " << host << std::endl;
        close(sock);
        return;
    }

    // ソケットをノンブロッキングモードに設定
    if (!set_socket_non_blocking(sock, true)) {
        std::cerr << "エラー: ソケットをノンブロッキングモードに設定できませんでした。\n";
        close(sock);
        return;
    }

    std::cout << "WPFアプリケーション(" << host << ":" << port << ")に接続を試行中...\n";
    
    int ret = connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        if (errno == EINPROGRESS) {
            // ノンブロッキング接続が進行中。select()で完了を待つ
            fd_set writefds;
            FD_ZERO(&writefds);
            FD_SET(sock, &writefds);

            // 接続タイムアウトを設定
            struct timeval connect_timeout;
            connect_timeout.tv_sec = 5;
            connect_timeout.tv_usec = 0;

            int activity = select(sock + 1, nullptr, &writefds, nullptr, &connect_timeout);
            if (activity > 0) {
                // ソケットが書き込み可能になった。接続結果を確認する
                int so_error;
                socklen_t len = sizeof(so_error);
                getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
                if (so_error != 0) {
                    std::cerr << "エラー: WPFアプリケーション(" << host << ":" << port << ")に接続できませんでした。 " 
                              << strerror(so_error) << std::endl;
                    close(sock);
                    return;
                }
                // 接続成功
            } else {
                // select()が0を返した場合はタイムアウト、-1の場合はエラー
                std::cerr << "エラー: WPFアプリケーション(" << host << ":" << port << ")への接続がタイムアウトまたは失敗しました。" << std::endl;
                close(sock);
                return;
            }
        } else {
            // EINPROGRESS以外の即時エラー
            std::cerr << "エラー: WPFアプリケーション(" << host << ":" << port << ")に接続できませんでした。 " 
                      << strerror(errno) << std::endl;
            close(sock);
            return;
        }
    }
    // ret == 0 の場合、即座に接続が完了した

    std::cout << "WPFアプリケーションに接続しました。設定を送信します...\n";
    std::string config_str = serialize_config();
    
    ssize_t total_sent = 0;
    const char* data_ptr = config_str.c_str();
    size_t data_len = config_str.length();

    while (total_sent < (ssize_t)data_len && !g_shutdown_flag.load()) {
        ssize_t bytes_sent = send(sock, data_ptr + total_sent, data_len - total_sent, 0);
        if (bytes_sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 一時的な送信不可、少し待って再試行
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            std::cerr << "エラー: データ送信に失敗しました。 " << strerror(errno) << std::endl;
            close(sock);
            return;
        }
        total_sent += bytes_sent;
    }

    if (g_shutdown_flag.load()) {
        std::cout << "送信がキャンセルされました。\n";
    } else {
        std::cout << "設定を送信しました（" << total_sent << " バイト）\n";
    }
    
    close(sock);
    std::cout << "接続を閉じました。\n";
}

/**
 * @brief WPFからの設定更新を待ち受けるサーバーとして動作する (別スレッドで実行)
 */
void handle_client_connection(int client_sock, const std::string& config_path); // プロトタイプ宣言

void receive_config_updates(const std::string& config_path) {
    std::string port_str = get_config_value("CONFIG_SYNC", "CPP_RECV_PORT", "12348");

    int port;
    try {
        port = std::stoi(port_str);
        if (port <= 0 || port > 65535) {
            throw std::out_of_range("ポート番号が範囲外です");
        }
    } catch (const std::exception& e) {
        std::cerr << "エラー: 不正なポート番号: " << port_str << " (" << e.what() << ")" << std::endl;
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
                if (!g_shutdown_flag.load()) {
                    std::cerr << "エラー: acceptに失敗しました。 " << strerror(errno) << std::endl;
                }
                continue;
            }

            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
            std::cout << "クライアント " << client_ip << ":" << ntohs(client_addr.sin_port) << " から接続を受信しました。\n";

            // 接続処理を別関数に委譲
            handle_client_connection(client_sock, config_path);
        }
    }

    close(listen_sock);
    std::cout << "設定更新受信スレッドを終了しました。\n";
}

/**
 * @brief 既存のソケットを通じて現在の設定を送信する
 * @param sock 既に接続済みのクライアントソケット
 */
void send_config_on_existing_socket(int sock) {
    std::string config_str = serialize_config();
    
    ssize_t total_sent = 0;
    const char* data_ptr = config_str.c_str();
    size_t data_len = config_str.length();

    // handle_client_connectionではノンブロッキングに設定していないため、sendはブロックするはず
    while (total_sent < (ssize_t)data_len && !g_shutdown_flag.load()) {
        ssize_t bytes_sent = send(sock, data_ptr + total_sent, data_len - total_sent, 0);
        if (bytes_sent < 0) {
            // EAGAIN/EWOULDBLOCKはブロッキングソケットでは通常発生しないが、念のため
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            std::cerr << "エラー: 設定の返信に失敗しました。 " << strerror(errno) << std::endl;
            return;
        }
        total_sent += bytes_sent;
    }

    if (g_shutdown_flag.load()) {
        std::cout << "設定の返信がキャンセルされました。\n";
    } else {
        std::cout << "設定を返信しました（" << total_sent << " バイト）\n";
    }
}

/**
 * @brief クライアントからの接続を処理し、完全なメッセージを受信する (改良版)
 * @param client_sock クライアントのソケットディスクリプタ
 */
void handle_client_connection(int client_sock, const std::string& config_path) {
    // クライアントソケットにもタイムアウトを設定
    struct timeval timeout;
    timeout.tv_sec = 10;  // 受信用は少し長めに設定
    timeout.tv_usec = 0;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    try {
        // 1. ヘッダー（メッセージ長）を改行まで読み込む
        std::string header;
        char c;
        int header_read_count = 0;
        const int MAX_HEADER_LENGTH = 20; // ヘッダーの最大長を制限

        while (recv(client_sock, &c, 1, 0) > 0 && !g_shutdown_flag.load()) {
            if (c == '\n') {
                break;
            }
            header += c;
            header_read_count++;
            
            // 異常に長いヘッダーを防ぐ
            if (header_read_count > MAX_HEADER_LENGTH) {
                std::cerr << "エラー: ヘッダーが長すぎます。\n";
                close(client_sock);
                return;
            }
        }

        if (header.empty() || g_shutdown_flag.load()) {
            close(client_sock);
            return;
        }

        // 2. メッセージ長をパースし、その長さのデータを受信する
        size_t expected_length = std::stoull(header);
        
        // 0バイトデータは「設定要求」として扱う
        if (expected_length == 0) {
            std::cout << "\nWPFから設定要求（0バイト）を受信しました。現在の設定を返信します。\n";
            send_config_on_existing_socket(client_sock);
            close(client_sock);
            return;
        }

        // 異常に大きなメッセージサイズを防ぐ
        const size_t MAX_MESSAGE_SIZE = 1024 * 1024; // 1MB
        if (expected_length > MAX_MESSAGE_SIZE) {
            std::cerr << "エラー: メッセージサイズが大きすぎます: " << expected_length << " bytes\n";
            close(client_sock);
            return;
        }
        
        std::string received_data;
        received_data.reserve(expected_length);
        
        std::vector<char> buffer(4096);
        size_t total_received = 0;

        while (total_received < expected_length && !g_shutdown_flag.load()) {
            size_t to_read = std::min(buffer.size(), expected_length - total_received);
            ssize_t bytes_received = recv(client_sock, buffer.data(), to_read, 0);
            
            if (bytes_received <= 0) {
                if (bytes_received == 0) {
                    std::cerr << "エラー: クライアントが接続を閉じました。" << std::endl;
                } else {
                    std::cerr << "エラー: データ受信中にエラーが発生しました: " << strerror(errno) << std::endl;
                }
                close(client_sock);
                return;
            }
            
            received_data.append(buffer.data(), bytes_received);
            total_received += bytes_received;
        }
        
        if (!g_shutdown_flag.load()) {
            std::cout << "\nWPFから設定データを受信しました（" << total_received << " バイト）\n";
            update_config_from_string(received_data);
            save_config(config_path); // ★変更点: 受信後すぐにファイルに保存
        }
        
    } catch (const std::exception& e) {
        std::cerr << "エラー: クライアント接続処理中に例外が発生しました: " << e.what() << std::endl;
    }
    
    close(client_sock);
}

/**
 * @brief 設定ファイルに現在の設定を保存する (改良版)
 * @param filename 保存先ファイル名
 */
void save_config(const std::string& filename) {
    std::lock_guard<std::mutex> lock(g_config_mutex);
    
    // バックアップファイルを作成
    std::string backup_filename = filename + ".backup";
    std::ifstream original(filename);
    if (original.good()) {
        std::ofstream backup(backup_filename);
        backup << original.rdbuf();
        original.close();
        backup.close();
        std::cout << "バックアップファイルを作成しました: " << backup_filename << "\n";
    }
    
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "エラー: 設定ファイル " << filename << " を書き込み用に開けませんでした。\n";
        return;
    }

    // コメントヘッダーを追加
    file << "# Navigator C++制御アプリケーションの設定ファイル\n";
    file << "# ConfigSynchronizerによって自動生成されました\n";
    file << "# 生成日時: " << std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() << "\n\n";

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
 * @brief 現在の設定を表示する (改良版)
 */
void print_current_config() {
    std::lock_guard<std::mutex> lock(g_config_mutex);
    std::cout << "\n=== 現在の設定 ===\n";
    
    // セクション名をソートして表示
    std::vector<std::string> section_names;
    for (const auto& section_pair : g_config_data) {
        section_names.push_back(section_pair.first);
    }
    std::sort(section_names.begin(), section_names.end());
    
    for (const std::string& section_name : section_names) {
        const auto& section_data = g_config_data.at(section_name);
        std::cout << "[" << section_name << "]\n";
        
        // キー名もソートして表示
        std::vector<std::string> key_names;
        for (const auto& key_value_pair : section_data) {
            key_names.push_back(key_value_pair.first);
        }
        std::sort(key_names.begin(), key_names.end());
        
        for (const std::string& key_name : key_names) {
            std::cout << "  " << key_name << " = " << section_data.at(key_name) << "\n";
        }
        std::cout << "\n";
    }
    std::cout << "==================\n\n";
}

/**
 * @brief 設定統計情報を表示する
 */
void print_config_stats() {
    std::lock_guard<std::mutex> lock(g_config_mutex);
    std::cout << "\n=== 設定統計情報 ===\n";
    std::cout << "セクション数: " << g_config_data.size() << "\n";
    
    int total_keys = 0;
    for (const auto& section_pair : g_config_data) {
        std::cout << "  [" << section_pair.first << "]: " << section_pair.second.size() << " 項目\n";
        total_keys += section_pair.second.size();
    }
    
    std::cout << "総キー数: " << total_keys << "\n";
    std::cout << "================\n\n";
}

int main(int argc, char* argv[]) {
    // シグナルハンドラーを設定
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    std::cout << "ConfigSynchronizer - Navigator制御システム設定同期ツール\n";
    std::cout << "============================================================\n";
    
    // config.iniのパスを指定
    std::string config_path = "config.ini";
    if (argc > 1) {
        config_path = argv[1];
    }

    std::cout << "設定ファイル: " << config_path << "\n\n";

    // 初期設定をファイルから読み込む
    if (!load_config(config_path)) {
        return 1;
    }

    // 読み込んだ設定の統計を表示
    print_config_stats();

    // WPFからの設定更新を待ち受けるスレッドを開始
    std::thread receiver_thread(receive_config_updates, config_path);

    // 少し待ってから、最初の設定をWPFに送信
    std::this_thread::sleep_for(std::chrono::seconds(1));
    send_config_to_wpf();

    std::cout << "\nメインの処理を実行中...\n";
    std::cout << "コマンド:\n";
    std::cout << "  Enter: 現在設定を再送信\n";
    std::cout << "  s: 設定を表示\n";
    std::cout << "  t: 設定統計を表示\n";
    std::cout << "  w: 現在の設定を " << config_path << " に上書き保存\n";
    std::cout << "  r: 設定ファイルを再読み込み\n";
    std::cout << "  q: 終了\n\n";

    // メインスレッドでは、他の処理を実行できる
    // ここではデモとして、ユーザー入力に応じて設定を再送信する
    while (!g_shutdown_flag.load()) {
        std::string line;
        std::getline(std::cin, line);
        
        if (g_shutdown_flag.load()) {
            break;
        }
        
        if (line == "q") {
            break;
        } else if (line == "s") {
            print_current_config();
        } else if (line == "t") {
            print_config_stats();
        } else if (line == "w") {
            save_config(config_path);
        } else if (line == "r") {
            std::cout << "設定ファイルを再読み込みしています...\n";
            if (load_config(config_path)) {
                std::cout << "設定ファイルの再読み込みが完了しました。\n";
                print_config_stats();
                // 再読み込み後、WPFに更新された設定を送信
                send_config_to_wpf();
            } else {
                std::cout << "設定ファイルの再読み込みに失敗しました。\n";
            }
        } else {
            std::cout << "現在の設定をWPFに再送信します。\n";
            send_config_to_wpf();
        }
    }

    // 終了処理
    std::cout << "\n終了処理中...\n";
    g_shutdown_flag.store(true);
    
    if (receiver_thread.joinable()) {
        std::cout << "受信スレッドの終了を待機中...\n";
        receiver_thread.join();
    }

    std::cout << "プログラムを終了します。\n";
    return 0;
}