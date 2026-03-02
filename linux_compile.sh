#!/bin/bash
echo "Welcome to Use NEKOPARTY Linux build script!"
echo "Updating godot ..."
git pull
git submodule update --init --recursive
echo "Start Compiling..."
scons platform=linuxbsd
echo "Copying Steam Libraries ..."
cp modules/godotsteam/sdk/redistributable_bin/linux64/libsteam_api.so bin/
echo "Build Complete! Find your build in the 'bin' folder."
