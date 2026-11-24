#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="build"

# Project sources: tracked files only; everything vendored in third-party/ is untouched
mapfile -t sources < <(git ls-files '*.c' '*.cpp' '*.h' '*.hpp' | grep -v '^third-party/')

# The gate only sees tracked files (CI validates the committed tree):
# new sources that were never `git add`ed would skip format and tidy.
mapfile -t orphans < <(git ls-files --others --exclude-standard -- '*.c' '*.cpp' '*.h' '*.hpp' \
    | grep -v '^third-party/' || true)
if ((${#orphans[@]})); then
    echo "ERROR: untracked C/C++ sources would skip the gate (git add them first):"
    printf '  %s\n' "${orphans[@]}"
    exit 1
fi

echo "=== 1. Formatting ==="
if ((${#sources[@]})); then
    clang-format -i "${sources[@]}"
fi

echo "=== 2. Building ==="
cmake -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Debug > /dev/null
ninja -C "$BUILD_DIR"

echo "=== 3. Static Analysis ==="
# tidy for production code only: tests with vendored utest.h are excluded
mapfile -t tidy_srcs < <(printf '%s\n' "${sources[@]}" | grep -v '/tests/' | grep -v '^$' || true)
if ((${#tidy_srcs[@]})); then
    clang-tidy -p "$BUILD_DIR" "${tidy_srcs[@]}" --quiet
fi

echo "=== 4. Running Tests ==="
ctest --test-dir "$BUILD_DIR" --output-on-failure

echo "=== 5. Auditing ELF Symbols & Dependencies ==="
mapfile -t libs < <(find "$BUILD_DIR" -type f -name '*.so')
for so in "${libs[@]}"; do
    if readelf -d "$so" | grep -q "NEEDED"; then
        echo "ERROR: $so depends on external shared libraries:"
        readelf -d "$so" | grep "NEEDED"
        exit 1
    fi
    if imports=$(llvm-nm -u "$so" | grep -E "alloc|memalign|strdup|free|_Zn|_Zd"); then
        echo "ERROR: forbidden heap allocations detected in $so:"
        echo "$imports"
        exit 1
    fi
    # Exports: plugin ABI only (plus linker bookkeeping symbols)
    if bad_exports=$(readelf --dyn-syms -W "$so" \
        | awk '$7 != "UND" && $4 ~ /^(FUNC|OBJECT)$/ && NF >= 8 {print $8}' \
        | grep -vE '^plugin_|^_+(bss_start|edata|end|data_start|data)$'); then
        echo "ERROR: $so exports non-plugin symbols:"
        echo "$bad_exports"
        exit 1
    fi
done
echo "audited ${#libs[@]} shared lib(s)"

echo "=== ALL CHECKS PASSED ==="
