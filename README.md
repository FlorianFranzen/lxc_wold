lxc_wold
========

A wake on lan daemon that allows you to start linux containers via wake on lan.

## Requirements
Requires the ```lxc-dev``` package to be installed. Newer versions of the package do not seem to include the needed headers anymore, so you might have to download the source yourself and add it to the CMakeLists.txt include directories.

## Build
```
mkdir build
cd build
cmake ..
make
```

Binaries can then be found in ```bin```
