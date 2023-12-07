# Compile QEMU
```sh
cd PATH_TO_QEMU
./configure --target-list=riscv64-softmmu,riscv64-linux-user --enable-slirp --enable-debug
make -jN
```

# Boot Ubuntu

## Software Requirements
Install opensbi and uboot:
```sh
sudo apt install opensbi
sudo apt install u-boot-qemu
```

## Download Ubuntu Disk Image
You can use my disk image (https://github.com/cs471-risk/ubuntu-img) with cloudsuite and docker already installed.
You need 15G+ disk space to unzip it.
```sh
tar xvf ubuntu.img.tar.gz
```
Then you will get ubuntu.img.

## QEMU Args
```sh
PATH_TO_QEMU/build/qemu-system-riscv64 -machine virt -m 8G -smp cpus=8 -nographic \
  -bios /usr/lib/riscv64-linux-gnu/opensbi/generic/fw_jump.bin \
  -kernel /usr/lib/u-boot/qemu-riscv64_smode/u-boot.bin \
  -plugin PATH_TO_QEMU/build/contrib/plugins/libhowvec.so -d plugin \
  -device virtio-net-device,netdev=eth0 -netdev user,id=eth0,hostfwd=tcp::55556-:22,hostfwd=tcp::55553-:443 \
  -drive file=ubuntu.img,format=raw,if=virtio
```
After running above command, you will see the ubuntu booting process, and then the login prompt.

User name: root
Password: 1234

# Running Benchmarks
In ubuntu's shell inside QEMU, execute the `set_flag` executable before running docker:
```sh
cd cloudsuite
./set_flag && DOCKER_COMMAND
```
After running the benchmark, kill the qemu process, then the instruction stats will be printed before qemu process terminated.

## Port Forwarding
You may need to use `hostfwd` to forward a host port to QEMU, for example, if server process inside QEMU listens on port 443, you can make client process send requests to port 55553, and forward it to 443 by `hostfwd=tcp::55553-:443`.
