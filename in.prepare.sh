#!/bin/bash

set -x

if [ HAVE_VMPI == "y" ]; then
    # prepare VMPI-KVM
    pushd .
    cd kernel/vmpi
    ./prepare-host-kvm.sh
    ./prepare-guest-kvm.sh
    popd
fi

sudo insmod kernel/rinalite.ko
sudo insmod kernel/rinalite-shim-loopback.ko
if [ HAVE_VMPI == "y" ]; then
    sudo insmod kernel/rinalite-shim-hv.ko
fi
sudo insmod kernel/rinalite-shim-eth.ko
sudo insmod kernel/rinalite-normal.ko
sudo chmod a+rwx /dev/rinalite
sudo chmod a+rwx /dev/rina-io

sudo mkdir -p /var/rinalite
sudo chmod -R a+rwx /var/rinalite