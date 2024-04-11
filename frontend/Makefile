TARGETS = frontendserver frontendlb loadbalancer

all: $(TARGETS)

frontendserver: frontendserver.cc
	g++ -std=c++17 $^ -lpthread -g -o $@

frontendlb: frontendlb.cc
	g++ -std=c++17 $^ -lpthread -g -o $@

loadbalancer: loadbalancer.cc
	g++ -std=c++17 $^ -lpthread -g -o $@

clean::
	rm -fv $(TARGETS) *~