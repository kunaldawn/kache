# kache - container image
#
# Two stages so the runtime carries a static-ish binary and nothing else.
# The store lives on a volume: it is a real file and wants to outlive the
# container, which is the whole point of a file backed cache.
FROM docker.io/library/alpine:3.20 AS build
RUN apk add --no-cache build-base
WORKDIR /src
COPY . .
RUN make distclean 2>/dev/null; make

FROM docker.io/library/alpine:3.20
RUN adduser -D -u 1000 kache && mkdir -p /data && chown kache /data
COPY --from=build /src/kache /usr/local/bin/kache
# The balancer ships in the same image: it is one small binary and
# having it here means the cluster needs no second image to front it.
COPY --from=build /src/kache-lb /usr/local/bin/kache-lb
USER kache
VOLUME /data
EXPOSE 7070
ENTRYPOINT ["/usr/local/bin/kache"]
CMD ["-f", "/data/kache.db", "-s", "1G", "-p", "7070", "-l", "0.0.0.0"]
