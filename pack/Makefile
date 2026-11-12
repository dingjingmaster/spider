.PHONY: all packages build test rootfs emerge deb rpm install-gentoo clean distclean

osArch                   := $(shell uname -m)
CC                       ?= gcc
CXX                      ?= g++
STRIP                    ?= strip
JOBS                     ?= $(shell nproc 2>/dev/null || echo 4)
PACK_DIR                 := $(abspath .)
PROJECT_DIR              := $(abspath ..)
BUILD_DIR                := $(PACK_DIR)/build/spider
VERSION                  := $(shell awk '/^project/ { print $$3; exit }' $(PROJECT_DIR)/CMakeLists.txt)

ifeq ($(osArch),x86_64)
osArch                    = amd64
else ifeq ($(osArch),aarch64)
osArch                    = arm64
endif

ifeq ($(filter $(osArch),amd64 arm64),)
$(error "Invalid arch: $(osArch)")
endif

ifeq ($(strip $(VERSION)),)
$(error "Cannot parse project version from $(PROJECT_DIR)/CMakeLists.txt")
endif

$(info "Current arch: $(osArch)")
$(info "Current version: $(VERSION)")


all:packages
	@echo "Build all packages!"


build:
	@echo Arch: ${osArch}, CC: ${CC}, CXX: ${CXX}
	@cmake -S "$(PROJECT_DIR)" -B "$(BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_C_COMPILER="$(CC)" \
		-DSPIDER_SANITIZE=OFF
	@cmake --build "$(BUILD_DIR)" --parallel "$(JOBS)"


test: build
	@ctest --test-dir "$(BUILD_DIR)" --output-on-failure


rootfs: build
	@STRIP="$(STRIP)" sh src/build-rootfs.sh "$(osArch)" "$(VERSION)"


packages: emerge deb rpm


emerge: rootfs
	@sh src/build-emerge.sh "$(osArch)" "$(VERSION)"


install-gentoo:
	@sh tools/install-gentoo.sh


deb: rootfs
	@sh src/build-deb.sh "$(osArch)" "$(VERSION)"


rpm: rootfs
	@sh src/build-rpm.sh "$(osArch)" "$(VERSION)"


clean:
	@rm -rf "$(PACK_DIR)/build"


distclean: clean
	@rm -rf "$(PACK_DIR)/dist"
