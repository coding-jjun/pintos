cd threads
make clean
make
cd build
pintos --gdb -- -q run $1