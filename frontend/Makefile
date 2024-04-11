TARGETS = frontendserver frontendlb

all: $(TARGETS)

frontendserver: frontendserver.cc
	g++ -std=c++17 $^ -lpthread -g -o $@

frontendlb: frontendlb.cc
	g++ -std=c++17 $^ -lpthread -g -o $@

clean::
	rm -fv $(TARGETS) *~