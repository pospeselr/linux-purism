#!/bin/sh
set -e
clear
basedir=$(echo $PWD)

# get source and cd into it.
echo "Removing previous build files..."
rm -rf linux-pureos
echo
mkdir -p linux-pureos
cp data/* linux-pureos -ar
cd linux-pureos

# go back to the base directory and build.
echo "Building linux..."
export CONCURRENCY_LEVEL=4
fakeroot make-kpkg --jobs=4 --initrd kernel_image kernel_headers modules_image
cd $basedir
