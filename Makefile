CXX      ?= g++
PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin
PAMDIR   ?= /etc/pam.d
VERSION  ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

CXXFLAGS ?= -O2
CXXFLAGS += -std=c++20 -Wall -Wextra -Wpedantic -MMD -MP \
            -fPIE -fstack-protector-strong -D_FORTIFY_SOURCE=3 -pthread \
            -DVKEEP_VERSION='"$(VERSION)"'
LDFLAGS  += -pthread -pie -Wl,-z,relro,-z,now $(EXTRA_LDFLAGS)
LDLIBS   += -lpam

SRCS := $(wildcard src/*.cpp)
OBJS := $(SRCS:src/%.cpp=build/%.o)
TEST_OBJS := $(SRCS:src/%.cpp=build/testing/%.o)
COVERAGE_OBJS := $(SRCS:src/%.cpp=build/coverage/%.o)
COVERAGE_MIN ?= 80
GCOVR ?= uvx --python 3.12 gcovr==8.*

all: build/vkeep

build/vkeep: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Binaire de test uniquement : lit sa configuration PAM dans un dossier choisi
# par l'environnement. Il ne doit jamais être installé.
build/vkeep_test: $(TEST_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/testing/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -DVKEEP_TESTING -c -o $@ $<

# Binaire de test instrumenté : -O0 pour des lignes fidèles, donc sans _FORTIFY_SOURCE.
build/vkeep_coverage: $(COVERAGE_OBJS)
	$(CXX) $(LDFLAGS) --coverage -o $@ $^ $(LDLIBS)

build/coverage/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -O0 -g -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 --coverage -DVKEEP_TESTING -c -o $@ $<

coverage: build/vkeep_coverage
	find build/coverage -name '*.gcda' -delete
	python3 tests/pty_test.py build/vkeep_coverage
	$(GCOVR) --root . --filter 'src/' --object-directory build/coverage \
	    --print-summary --txt - --fail-under-line $(COVERAGE_MIN)

test: build/vkeep_test
	python3 tests/pty_test.py build/vkeep_test
	python3 tests/scripts_test.py

vm-test: build/vkeep
	python3 tests/vm_test.py build/vkeep

install: build/vkeep
	install -Dm755 build/vkeep $(DESTDIR)$(BINDIR)/vkeep
	install -Dm644 pam/vkeep $(DESTDIR)$(PAMDIR)/vkeep

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/vkeep $(DESTDIR)$(PAMDIR)/vkeep

# Archive de release : le binaire et de quoi l'installer, avec les mêmes chemins que le dépôt.
DIST_NAME = vkeep-$(VERSION)-linux-$(shell uname -m)
dist: build/vkeep
	rm -rf build/dist
	mkdir -p build/dist/$(DIST_NAME)/build build/dist/$(DIST_NAME)/scripts
	install -m755 build/vkeep build/dist/$(DIST_NAME)/build/vkeep
	strip build/dist/$(DIST_NAME)/build/vkeep
	cp -r pam README.md LICENSE build/dist/$(DIST_NAME)/
	cp -r scripts/install.sh scripts/uninstall.sh scripts/lib build/dist/$(DIST_NAME)/scripts/
	tar -C build/dist -czf build/dist/$(DIST_NAME).tar.gz $(DIST_NAME)
	cd build/dist && sha256sum $(DIST_NAME).tar.gz > $(DIST_NAME).tar.gz.sha256

clean:
	rm -rf build

.PHONY: all test coverage vm-test dist install uninstall clean

-include $(OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(COVERAGE_OBJS:.o=.d)
