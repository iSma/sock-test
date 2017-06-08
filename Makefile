BIN = sock-test
CC ?= gcc

CFLAGS = -std=gnu11 -Wall -Wextra -g
LFLAGS = -lpthread

SRC_DIR = src
OBJ_DIR = build
BIN_DIR = bin


REPO     = localhost:5000
IMAGE    = $(BIN)
SERVICE  = $(BIN)
NETWORK  = $(SERVICE)-net
VOLUME   = $(SERVICE)-vol
REPLICAS = 100
SERVICE_OPTIONS = --restart-condition=none

TRACKER_URL = "tasks.$(SERVICE)"
CMD = -cs -i4 -mUDP

# =============================

SRC = $(wildcard $(SRC_DIR)/*.c)
OBJ = $(SRC:%.c=$(OBJ_DIR)/%.o)
DEP = $(OBJ:%.o=%.d)

default: $(BIN)

run: $(BIN)
	$(BIN_DIR)/$(BIN)

$(BIN): $(BIN_DIR)/$(BIN)

$(BIN_DIR)/$(BIN): $(OBJ)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ -o $@ $(LFLAGS)

-include $(DEP)

$(OBJ_DIR)/%.o : %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -c $< -o $@

.PHONY: clean docker image service network volume logs
clean:
	-@rm -Rd "$(BIN_DIR)" "$(OBJ_DIR)" 2> /dev/null

clean-docker: stop
	-docker volume rm "$(VOLUME)"
	-docker network rm "$(NETWORK)"
	-docker rmi "$(REPO)/$(IMAGE)"

clean-logs:
	-docker run -v "$(VOLUME)":"/logs" -w"/logs" busybox \
		find -name '*.txt' -exec rm '{}' +

start: image service

image: $(BIN)
	docker build -t "$(REPO)/$(IMAGE)" .
	docker push "$(REPO)/$(IMAGE)"

stop:
	-docker service rm "$(SERVICE)" 2> /dev/null

service: stop network volume
	docker service create \
		--name "$(SERVICE)" \
		--network "$(NETWORK)" \
		--mount type=volume,source="$(VOLUME)",destination="/logs" \
		--replicas "$(REPLICAS)" \
		$(SERVICE_OPTIONS) \
		"$(REPO)/$(IMAGE)" -f/logs -u"$(TRACKER_URL)" $(CMD)

network:
	@docker network ls --format '{{.Name}}' | grep -qx "$(NETWORK)" \
		|| docker network create --driver overlay "$(NETWORK)"

volume:
	@docker volume ls -q | grep -qx "$(VOLUME)" \
		|| docker volume create --name "$(VOLUME)"

logs: volume
	@docker run -v "$(VOLUME)":"/logs" -w"/logs" busybox \
		find -name '*.txt' -exec cat '{}' +
