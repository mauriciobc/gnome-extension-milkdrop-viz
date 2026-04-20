#!/bin/bash
set -e

echo "Applying patch to projectM..."
cd reference_codebases/projectm

# Check if already patched to avoid double-patching failure
if ! grep -q "case HLSLBaseType_Float4: dim = 4; break;" vendor/hlslparser/src/GLSLGenerator.cpp; then
    patch -p1 < ../../tools/projectm-hlsl-array-fix.patch
else
    echo "Patch already applied."
fi

echo "Building projectM..."
mkdir -p build
cd build
# Install to ~/.local to avoid requiring sudo
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$HOME/.local
make -j$(nproc)

echo "Installing projectM locally..."
make install

echo "ProjectM successfully installed to $HOME/.local"
