TARGETS = frontendserver loadbalancer

all: $(TARGETS)

frontendserver: frontendserver.cc routes.cc
	g++ -std=c++17 $(shell pkg-config --cflags uuid) $^ $(shell pkg-config --libs uuid) -lpthread -g -o $@

loadbalancer: loadbalancer.cc
	g++ -std=c++17 $^ -lpthread -g -o $@

clean::
	rm -fv $(TARGETS) *~