FROM gcc:latest as build

RUN apt-get update && apt-get install -y postgresql-client

WORKDIR /app

COPY API.c .
COPY cjson/ cjson/

RUN gcc -o server cjson/cJSON.c API.c -lpq

FROM debian:12.4-slim

WORKDIR /work/

COPY --from=build /app/server .

RUN apt-get update && apt-get install -y postgresql-client

EXPOSE 8080

CMD ["./server"]
