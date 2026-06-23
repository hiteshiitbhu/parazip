#!/bin/bash
set -e

# Colors for terminal output
RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

echo -e "${CYAN}=== ParaZip Automated Test Suite ===${NC}"

# Clean up previous tests
rm -rf test_workspace extraction_workspace test_archive.pzip
mkdir -p test_workspace/dir1/dir2

echo "1. Generating test files..."
# Create a small text file
echo "Hello, this is a small text file for basic testing." > test_workspace/small.txt

# Create a highly compressible file (repeating text)
python3 -c "print('Antigravity Parallel Zip Systems Project C ' * 150000)" > test_workspace/dir1/compressible.txt

# Create a raw binary file using /dev/urandom (incompressible binary)
dd if=/dev/urandom of=test_workspace/dir1/dir2/random.bin bs=1M count=3 2>/dev/null

# Create empty file
touch test_workspace/empty.txt

# Set specific permissions to test permission preservation
chmod 755 test_workspace/small.txt
chmod 644 test_workspace/dir1/compressible.txt
chmod 700 test_workspace/dir1/dir2/random.bin

echo "2. Compressing test_workspace/ using 4 worker threads..."
./pzip -c -t 4 test_archive.pzip test_workspace

echo -e "\n3. Listing archive details..."
./pzip -l test_archive.pzip

echo -e "\n4. Setting up extraction directory..."
mkdir -p extraction_workspace
cp test_archive.pzip extraction_workspace/
cd extraction_workspace

echo "5. Extracting archive..."
../pzip -x test_archive.pzip
rm test_archive.pzip
cd ..

echo -e "\n6. Verifying data integrity and permission match..."
if diff -r test_workspace extraction_workspace/test_workspace; then
    echo -e "${GREEN}[SUCCESS] All extracted files match the originals byte-for-byte!${NC}"
else
    echo -e "${RED}[FAILURE] Extracted file contents do not match originals!${NC}"
    exit 1
fi

# Verify permissions
perm_orig=$(stat -c "%a" test_workspace/dir1/dir2/random.bin)
perm_extr=$(stat -c "%a" extraction_workspace/test_workspace/dir1/dir2/random.bin)

if [ "$perm_orig" = "$perm_extr" ]; then
    echo -e "${GREEN}[SUCCESS] File permissions were preserved successfully (${perm_orig})!${NC}"
else
    echo -e "${RED}[FAILURE] File permissions mismatch! Original: ${perm_orig}, Extracted: ${perm_extr}${NC}"
    exit 1
fi

# Cleanup on success
echo -e "\n7. Cleaning up test workspace..."
rm -rf test_workspace extraction_workspace test_archive.pzip
echo -e "${GREEN}=== ALL TESTS PASSED SUCCESSFULLY! ===${NC}"
