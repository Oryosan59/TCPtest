# Makefile for ConfigSynchronizer on Raspberry Pi

# コンパイラとフラグ
CXX = g++
CXXFLAGS = -std=c++11 -Wall -Wextra -O2
LDFLAGS = -liniparser -lpthread

# ターゲット名
TARGET = ConfigSynchronizer
SOURCE = ConfigSynchronizer.cpp

# デフォルトターゲット
all: $(TARGET)

# メインターゲット
$(TARGET): $(SOURCE)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SOURCE) $(LDFLAGS)

# クリーンアップ
clean:
	rm -f $(TARGET)

# インストール（/usr/local/binにコピー）
install: $(TARGET)
	sudo cp $(TARGET) /usr/local/bin/
	sudo chmod 755 /usr/local/bin/$(TARGET)

# アンインストール
uninstall:
	sudo rm -f /usr/local/bin/$(TARGET)

# 依存関係チェック
check-deps:
	@echo "必要な依存関係をチェックしています..."
	@dpkg -l | grep -q libiniparser-dev || echo "libiniparser-devが見つかりません。sudo apt install libiniparser-devでインストールしてください。"
	@which g++ > /dev/null || echo "g++が見つかりません。sudo apt install build-essentialでインストールしてください。"

# 実行
run: $(TARGET)
	./$(TARGET)

# デバッグビルド
debug: CXXFLAGS += -g -DDEBUG
debug: $(TARGET)

# 静的解析
lint:
	@which cppcheck > /dev/null && cppcheck --enable=all --std=c++11 $(SOURCE) || echo "cppcheckが見つかりません。sudo apt install cppcheckでインストールしてください。"

# ヘルプ
help:
	@echo "利用可能なターゲット:"
	@echo "  all        - プログラムをビルド"
	@echo "  clean      - ビルドファイルを削除"
	@echo "  install    - /usr/local/binにインストール"
	@echo "  uninstall  - インストールを削除"
	@echo "  check-deps - 依存関係をチェック"
	@echo "  run        - ビルドして実行"
	@echo "  debug      - デバッグ情報付きでビルド"
	@echo "  lint       - 静的解析を実行"
	@echo "  help       - このヘルプを表示"

.PHONY: all clean install uninstall check-deps run debug lint help
