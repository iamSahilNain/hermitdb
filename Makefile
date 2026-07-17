# Dev loop for macOS hosts: every compile/test runs inside the Linux dev
# container (epoll does not exist on Darwin). Source is volume-mounted, the
# build/ tree lives in the repo (gitignored), so incremental builds are fast.

IMG        := hermitdb-dev
DOCKER_RUN := docker run --rm -v "$(PWD)":/work -w /work $(IMG)
DOCKER_TTY := docker run --rm -it -v "$(PWD)":/work -w /work $(IMG)

.PHONY: image configure build test test-all run shell cli fmt clean

image:            ## build the dev image (once, and after Dockerfile edits)
	docker build --target dev -t $(IMG) .

configure:
	$(DOCKER_RUN) cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo

build: 	          ## incremental build inside the container
	$(DOCKER_RUN) cmake --build build -j

test: build       ## scaffold + m3 tests — these must always be green
	$(DOCKER_RUN) ctest --test-dir build -L 'scaffold|m3' --output-on-failure

test-all: build   ## everything, incl. checkpoint tests (red until you implement them)
	$(DOCKER_RUN) ctest --test-dir build --output-on-failure

run: build        ## run the server on localhost:6380
	docker run --rm -it -p 6380:6380 -v "$(PWD)":/work -w /work $(IMG) ./build/hermitdb --port=6380

cli:              ## redis-cli against a `make run` server in another terminal
	$(DOCKER_TTY) redis-cli -h host.docker.internal -p 6380

shell:
	$(DOCKER_TTY) bash

fmt:
	$(DOCKER_RUN) bash -c 'find src tests -name "*.h" -o -name "*.cpp" | xargs clang-format -i'

clean:
	rm -rf build
