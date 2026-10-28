ROOT := $(abspath ..)
CXX ?= g++
BUILD ?= $(CURDIR)/build-host
CPPFLAGS += -Iinclude $(shell pkg-config --cflags sdl2 freetype2 libcurl jansson minizip libwebp)
CXXFLAGS += -std=c++17 -O2 -g -Wall -Wextra -Werror -MMD -MP
LDLIBS += $(shell pkg-config --libs sdl2 freetype2 libcurl jansson minizip libwebp) -lmbedcrypto -lz -pthread
SOURCES := drawing frontend http storage import catalog prepare repair loading heap mods nbt content versions images packwiz
OBJECTS := $(addprefix $(BUILD)/,$(addsuffix .o,$(SOURCES)))
.PHONY: all tests check ui-check http-check
all: $(BUILD)/mineswitch-launcher
$(BUILD):
	mkdir -p $@
$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
$(BUILD)/mineswitch-launcher: $(OBJECTS)
	$(CXX) $(LDFLAGS) $(OBJECTS) $(LDLIBS) -o $@
$(BUILD)/catalog_tests: tests/catalog_tests.cpp $(BUILD)/catalog.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -ljansson -pthread -o $@
$(BUILD)/storage_tests: tests/storage_tests.cpp $(BUILD)/storage.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -ljansson -pthread -o $@
$(BUILD)/import_tests: tests/import_tests.cpp $(BUILD)/storage.o $(BUILD)/import.o $(BUILD)/catalog.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -ljansson -lminizip -lmbedcrypto -lz -pthread -o $@
$(BUILD)/frontend-test.o: src/frontend.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DMINESWITCH_UI_TEST=1 -c $< -o $@
$(BUILD)/ui_smoke: $(BUILD)/frontend-test.o $(filter-out $(BUILD)/frontend.o,$(OBJECTS))
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@
ui-check: $(BUILD)/ui_smoke
	python3 tests/ui_smoke.py $(BUILD)/ui_smoke
$(BUILD)/http_tests: tests/http_tests.cpp $(BUILD)/http.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(shell pkg-config --libs libcurl) -lmbedcrypto -pthread -o $@
http-check: $(BUILD)/http_tests
	python3 tests/http_integration.py $(BUILD)/http_tests
$(BUILD)/sd_tool: tests/sd_tool.cpp $(filter-out $(BUILD)/frontend.o $(BUILD)/drawing.o,$(OBJECTS))
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDLIBS) -o $@
$(BUILD)/prepare_tests: tests/prepare_tests.cpp $(BUILD)/prepare.o $(BUILD)/storage.o $(BUILD)/import.o $(BUILD)/heap.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -ljansson -lminizip -lmbedcrypto -lz -pthread -o $@
$(BUILD)/repair_tests: tests/repair_tests.cpp $(BUILD)/repair.o $(BUILD)/storage.o $(BUILD)/import.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -ljansson -lminizip -lmbedcrypto -lz -pthread -o $@
$(BUILD)/heap_tests: tests/heap_tests.cpp $(BUILD)/heap.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@
$(BUILD)/mods_tests: tests/mods_tests.cpp $(BUILD)/mods.o $(BUILD)/content.o $(BUILD)/nbt.o $(BUILD)/catalog.o $(BUILD)/storage.o $(BUILD)/import.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -ljansson -lminizip -lmbedcrypto -lz -pthread -o $@
$(BUILD)/content_tests: tests/content_tests.cpp $(BUILD)/content.o $(BUILD)/nbt.o $(BUILD)/catalog.o $(BUILD)/storage.o $(BUILD)/import.o $(BUILD)/prepare.o $(BUILD)/heap.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -ljansson -lminizip -lmbedcrypto -lz -pthread -o $@
$(BUILD)/mods_live: tests/mods_live.cpp $(BUILD)/mods.o $(BUILD)/content.o $(BUILD)/nbt.o $(BUILD)/storage.o $(BUILD)/import.o $(BUILD)/catalog.o $(BUILD)/http.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -ljansson -lminizip -lmbedcrypto -lz -pthread $(shell pkg-config --libs libcurl) -o $@
$(BUILD)/packwiz_tests: tests/packwiz_tests.cpp $(BUILD)/packwiz.o $(BUILD)/catalog.o $(BUILD)/storage.o $(BUILD)/import.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -ljansson -lminizip -lmbedcrypto -lz -pthread -o $@
tests: $(BUILD)/packwiz_tests $(BUILD)/catalog_tests $(BUILD)/storage_tests $(BUILD)/import_tests $(BUILD)/prepare_tests $(BUILD)/repair_tests $(BUILD)/heap_tests $(BUILD)/mods_tests $(BUILD)/content_tests
check: tests
	$(BUILD)/catalog_tests
	$(BUILD)/storage_tests
	$(BUILD)/import_tests
	$(BUILD)/prepare_tests
	$(BUILD)/repair_tests
	$(BUILD)/heap_tests
	$(BUILD)/mods_tests
	$(BUILD)/content_tests
	$(BUILD)/packwiz_tests
-include $(OBJECTS:.o=.d) $(BUILD)/frontend-test.d
