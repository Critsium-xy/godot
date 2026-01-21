@echo off
echo Updating godot ...
git pull
git submodule update --init --recursive
echo Start Compiling...
scons platform=windows
pause