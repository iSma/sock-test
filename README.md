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
 
### Start the service
```sh
make start
```
 
### Stop the service
```sh
make stop
```

### Print logs
```sh
make logs
```

### Delete logs
```sh
make clean-logs
```

### Clean up volume, image, network
```sh
make clean-docker
```

### Configuration
The service can be configured by customizing variables in `Makefile` or
overriding their value in the command line:
```sh
make start REPLICAS=200 SERVICE_OPTIONS='--restart-condition=none --limit-memory=128MB'
```
