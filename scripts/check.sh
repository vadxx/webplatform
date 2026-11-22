#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="build"
SO_FRONT="$BUILD_DIR/frontend.so"
SO_BACK="$BUILD_DIR/backend.so"

echo "=== 1. Formatting ==="
find src lib tests -type f \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) \
    -exec clang-format -i {} +

echo "=== 2. Building ==="
cmake -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Debug > /dev/null
ninja -C "$BUILD_DIR"

echo "=== 3. Static Analysis ==="
clang-tidy -p "$BUILD_DIR" src/*.cpp lib/*.cpp --quiet

echo "=== 4. Running Tests ==="
ctest --test-dir "$BUILD_DIR" --output-on-failure

echo "=== 5. Auditing ELF Symbols & Dependencies ==="
if readelf -d "$SO_FRONT" | grep -q "NEEDED"; then
    echo "ERROR: $SO_FRONT depends on external shared libraries:"
    readelf -d "$SO_FRONT" | grep "NEEDED"
    exit 1
fi
if llvm-nm -u "$SO_FRONT" | grep -E "malloc|calloc|realloc|free|_Znw|_Zda"; then
    echo "ERROR: Forbidden heap allocations detected in $SO_FRONT:"
    llvm-nm -u "$SO_FRONT" | grep -E "malloc|calloc|realloc|free|_Znw|_Zda"
    exit 1
fi

if readelf -d "$SO_BACK" | grep -q "NEEDED"; then
    echo "ERROR: $SO_BACK depends on external shared libraries:"
    readelf -d "$SO_BACK" | grep "NEEDED"
    exit 1
fi
if llvm-nm -u "$SO_BACK" | grep -E "malloc|calloc|realloc|free|_Znw|_Zda"; then
    echo "ERROR: Forbidden heap allocations detected in $SO_BACK:"
    llvm-nm -u "$SO_BACK" | grep -E "malloc|calloc|realloc|free|_Znw|_Zda"
    exit 1
fi

echo "=== ALL CHECKS PASSED ==="