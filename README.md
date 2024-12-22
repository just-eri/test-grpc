mkdir -p cmake/build

pushd cmake/build

cmake -DCMAKE_PREFIX_PATH=$LOCAL_DIR ../..

make -j 4
