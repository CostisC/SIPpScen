vpath %.cpp src

# common source files
SRC 	:= src/shared_list.cpp
OBJ 	= $(notdir $(SRC:.cpp=.o))
HEADERS	:= $(wildcard h/*.h)
CXXFLAGS += -DMAX_NODES=2000
SERVER := media_server
CLIENT := media_endpoint
$(SERVER): LIBS += -lpistache -Wl,-rpath='$$ORIGIN'
$(SERVER): CXXFLAGS  += -DCLIENT=\"$(CLIENT)\"
$(CLIENT): LIBS += $(shell pkgconf --libs --static libpjproject) -lcurl
$(CLIENT): CXXFLAGS  += $(shell pkgconf --cflags libpjproject)
TEST_WRITER := test_writer
TEST_READER := test_reader
PACK := sippscen-pack
debug: CXXFLAGS += -g
SIPP ?= $(shell which sipp)



.PHONY :  all clean clean_obj test debug pack deb-pack \
          clean-old-packs

all: $(SERVER) $(CLIENT)


debug: all

test: $(TEST_WRITER) $(TEST_READER)


$(SERVER): $(OBJ) media_server.o
	$(CXX) $^ $(LIBS) -o $@

$(CLIENT): $(OBJ) rtp_endpoint.o media_endpoint.o influxdb_client.o
	$(CXX) $^ $(LIBS) -o $@

%.o: %.cpp $(HEADERS)
	@echo
	@echo "*** compile $@ ***"
	$(CXX) -Ih  $< $(CXXFLAGS) -c -o $@


$(TEST_WRITER): $(addsuffix .o,$(TEST_WRITER)) $(OBJ)
	$(CXX) $^ -o $@

$(TEST_READER): $(addsuffix .o,$(TEST_READER)) $(OBJ)
	$(CXX) $^ -o $@


clean:
	rm -f $(TEST_WRITER) $(TEST_READER) $(SERVER) $(CLIENT)
	$(MAKE) clean_obj

clean_obj:
	rm -f $(wildcard *.o)

install:
	install -D -m 755 $(SERVER) $(DESTDIR)/usr/bin/$(SERVER)
	install -D -m 755 $(CLIENT) $(DESTDIR)/usr/bin/$(CLIENT)
	install -D -m 755 libpistache.so.0 $(DESTDIR)/usr/bin/
	if [ -n "$(SIPP)" ]; then \
		[ -f $(SIPP) ] && install -D -m 755 $(SIPP) $(DESTDIR)/usr/bin/ ; \
	fi
	

pack:   all test
	@mkdir -p $(PACK)
	cp -v $(CLIENT) $(SERVER) $(TEST_READER) $(TEST_WRITER) $(PACK)
	-cp -v libpistache.so* $(PACK)
	-cp -v $(SIPP) $(PACK)
	@tar -cf $(PACK).tar $(PACK)
	@rm -rf $(PACK)
	@echo "================================"
	@echo "Executables packed in $(PACK).tar"

deb-pack:
	dch -i && dpkg-buildpackage -us -uc

clean-old-packs:
	 @set -- $$(awk '/\(.*\).*urgency/ {match($$0, "(.*) \\((.*)\\)", a); print a[1],a[2]; exit}' debian/changelog); \
		 find .. -maxdepth 1 | grep  "$$1" | grep -v "$$2[_.]" | xargs rm -f

