# sock-test

Simple program to illustrate [Docker issue #29992](https://github.com/moby/moby/issues/29992)

## Prerequisites
### Have a Docker Swarm running (no need for more than one node)
```sh
docker swarm init
```

### Run a registry
```sh
docker service create --publish 5000:5000 --name registry registry:2
```


## Usage
 
Run `make start` to start the service, `make stop` to stop it. Logs are saved
to a volume; run `make logs` to access a container with this volume mounted.
Run `make clean-docker` to remove the created volume, image and network.

The service can be configured by customizing variables in `Makefile` or
overriding their value in the command line:
```sh
make start REPLICAS=200 SERVICE_OPTIONS='
```
