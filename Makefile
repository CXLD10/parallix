# Convenience wrapper around CMake.
#   make            build the parallix executable (no test dependencies)
#   make test       build and run the full test suite
#   make install    install to PREFIX (default /usr/local)
#   make sanitize   run the test suite under AddressSanitizer + UBSan
PREFIX ?= /usr/local
BUILD  ?= build

.PHONY: all test install sanitize clean demo

all:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release -DPARALLIX_BUILD_TESTS=OFF
	cmake --build $(BUILD) -j

test:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release -DPARALLIX_BUILD_TESTS=ON
	cmake --build $(BUILD) -j
	ctest --test-dir $(BUILD) --output-on-failure -j4

install: all
	cmake --install $(BUILD) --prefix $(PREFIX)

sanitize:
	cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DPARALLIX_SANITIZE=ON -DPARALLIX_BUILD_TESTS=ON
	cmake --build build-san -j
	PARALLIX_CFLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined" \
	  ctest --test-dir build-san --output-on-failure -j4

demo: all
	./demo.sh

clean:
	cmake -E remove_directory $(BUILD)
	cmake -E remove_directory build-san
