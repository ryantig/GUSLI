#!/bin/bash
# Tested on ubuntu and ubuntu WSL in Windows11. Integration with spdk library for server side bdev
# Step 1.0: Install basic dependencies
sudo apt update sudo apt install -y git build-essential gcc make libcunit1-dev libaio-dev libssl-dev uuid-dev;
sudo apt install meson ninja-build;
sudo apt install -y libnuma-dev;
sudo apt install -y libncurses-dev;
sudo apt install -y python3-pip;
sudo apt install -y python3-pyelftools;
sudo apt install -y libcunit1 libcunit1-doc libcunit1-dev;
sudo apt install -y libisal-dev
#sudo apt-get install elfutils numactl

# Step 1.1: Clone SPDK/DPDK source
cd ~/projects;
git clone ssh://git@github.com/spdk/spdk.git;
cd spdk;
git submodule update --init;
# Step 1.2: Build DPDK
cd dpdk && meson build && ninja -C build && cd ..;
# Step 1.2: Build SPDK, For more info see: https://spdk.io/doc/getting_started.html
sudo scripts/pkgdep.sh;
./configure make

# Set Up Hugepages (required for DPDK):
sudo mkdir -p /mnt/huge;
sudo mount -t hugetlbfs nodev /mnt/huge;
sudo bash -c 'echo 4096 > /proc/sys/vm/nr_hugepages';

CONF_FILE=`realpath ../gusli/07examples/server/spdk_bdev.conf`;
cd ~/projects/spdk;
if false; then				# Optionally check that all unitests pass
	./test/unit/unittest.sh;
	sudo ./build/examples/hello_bdev -c ./examples/bdev/hello_world/bdev.json -b Malloc0;
	sudo ./build/examples/hello_bdev -c ${CONF_FILE} -b dhs_bdev;
	sudo ./build/examples/bdevperf -c ${CONF_FILE} -q 1 -o 4096 -w write -t 3;
fi
