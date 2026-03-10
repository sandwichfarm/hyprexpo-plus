# Else exist specifically for clang
ifeq ($(CXX),g++)
    EXTRA_FLAGS = --no-gnu-unique
else
    EXTRA_FLAGS =
endif

CXXFLAGS = -shared -fPIC -g -std=c++2b -Wno-c++11-narrowing -Wno-narrowing
INCLUDES = `pkg-config --cflags pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon`
LIBS = `pkg-config --libs pangocairo`

SRC = main.cpp overview.cpp ExpoGesture.cpp OverviewPassElement.cpp
TARGET = hyprexpo.so
INSTALL_DIR = /var/cache/hyprpm/$(USER)/hyprexpo-plus
INSTALL_NAME = hyprexpo-plus.so

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(EXTRA_FLAGS) $(INCLUDES) $^ -o $@ $(LIBS)

install: $(TARGET)
	install -Dm755 $(TARGET) $(INSTALL_DIR)/$(INSTALL_NAME)

clean:
	rm -f ./$(TARGET)

.PHONY: all clean install
