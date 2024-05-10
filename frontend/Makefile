
TARGETS = frontendserver loadbalancer smtpserver smtpclient

all: $(TARGETS)

frontendserver: frontendserver.cc routes.cc
	g++ -std=c++17 $(shell pkg-config --cflags uuid) -I/usr/local/opt/openssl/include $^ $(shell pkg-config --libs uuid) -L/usr/local/opt/openssl/lib -lcrypto -lpthread -lresolv -g -o $@
	 
loadbalancer: loadbalancer.cc
	g++ -std=c++17 $^ -lpthread -g -o $@

smtpserver: smtp-server.cc routes.cc
	g++ -std=c++17 $(shell pkg-config --cflags uuid) $^ -I/usr/local/opt/openssl/include $(shell pkg-config --libs uuid) -L/usr/local/opt/openssl/lib -lcrypto -lpthread -lresolv -g -o $@

smtpclient: smtp-client.cc
	g++ -std=c++17 $^ -lpthread -g -o $@
	
clean::
	rm -fv $(TARGETS) *~
