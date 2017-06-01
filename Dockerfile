# FROM ubuntu
# RUN apt-get update -y && apt-get upgrade -y && apt-get install -y build-essential
FROM alpine
RUN apk add --update build-base
RUN mkdir -p /opt/app/
WORKDIR /opt/app/
COPY Makefile .
COPY src ./src
RUN make
ENTRYPOINT ["./bin/sock-test"]
CMD -h
