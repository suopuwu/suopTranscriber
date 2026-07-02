BUILD_DIR := build

.PHONY: configure build run clean

configure:
	cmake -S . -B $(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=Release

build:
	cmake --build $(BUILD_DIR)

run: build
	./$(BUILD_DIR)/suopTranscriber

clean:
	rm -rf $(BUILD_DIR)
