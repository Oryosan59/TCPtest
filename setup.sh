#!/bin/bash
# setup.sh - Raspberry PiでConfigSynchronizerをセットアップするスクリプト

set -e  # エラーで停止

echo "=== ConfigSynchronizer Raspberry Pi セットアップ ==="

# システム更新
echo "システムパッケージを更新中..."
sudo apt update

# 必要なパッケージのインストール
echo "必要なパッケージをインストール中..."
sudo apt install -y build-essential libiniparser-dev pkg-config

# オプション: 開発ツールもインストール
read -p "開発ツール（gdb, valgrind, cppcheck）もインストールしますか？ (y/n): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "開発ツールをインストール中..."
    sudo apt install -y gdb valgrind cppcheck
fi

# ビルド
echo "プログラムをビルド中..."
make clean
make all

# 動作テスト
echo "ビルドが完了しました。"
echo "設定ファイル config.ini が存在するかチェック中..."

if [ ! -f "config.ini" ]; then
    echo "警告: config.ini が見つかりません。"
    echo "サンプル設定ファイルを作成しますか？ (y/n): "
    read -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        cat > config.ini << 'EOF'
[CONFIG_SYNC]
WPF_HOST=192.168.4.10
WPF_RECV_PORT=12347
CPP_RECV_PORT=12348

[PWM]
PWM_MIN=1100
PWM_NEUTRAL=1500
PWM_NORMAL_MAX=1500
PWM_BOOST_MAX=1900
PWM_FREQUENCY=50.0

[JOYSTICK]
DEADZONE=6500

[LED] 
CHANNEL=9
ON_VALUE=1900
OFF_VALUE=1100
EOF
        echo "サンプル config.ini を作成しました。"
    fi
fi

# 実行権限付与
chmod +x ConfigSynchronizer

echo ""
echo "=== セットアップ完了 ==="
echo "使用方法:"
echo "  ./ConfigSynchronizer          - 実行"  
echo "  ./ConfigSynchronizer config.ini - 設定ファイルを指定して実行"
echo "  make install                  - システムにインストール"
echo "  make help                     - その他のオプション"
echo ""

# インストールオプション
read -p "システムにインストールしますか？ (/usr/local/bin) (y/n): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    make install
    echo "インストール完了。どこからでも 'ConfigSynchronizer' コマンドで実行できます。"
fi

echo "セットアップが完了しました！"