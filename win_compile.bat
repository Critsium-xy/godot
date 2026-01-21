@echo off
echo Updating godot ...
git pull
git submodule update --init --recursive
echo Start Compiling...
scons platform=windows
echo Copying Steam Libraries ...
copy modules\godotsteam\sdk\redistributable_bin\win64\steam_api64.dll bin\
copy modules\godotsteam\sdk\redistributable_bin\win64\steam_api64.lib bin\
echo Build Complete! Find your build in the 'bin' folder.
pause