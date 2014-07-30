#!/bin/bash -e

#
# Copyright 2014, NICTA
#
# This software may be distributed and modified according to the terms of
# the BSD 2-Clause license. Note that NO WARRANTY is provided.
# See "LICENSE_BSD2.txt" for details.
#
# @TAG(NICTA_BSD)
#

# Echo all commands if V=3; maximum verbosity.
if [ 0${V} -ge 3 ]; then
    set -x
fi

# Check usage.
if [ $# -lt 2 ]; then
    echo "Usage: $0 <output file> <symbol name> [input files]"
    echo
    echo "Generate an '.o' file that contains the symbol 'symbol name'"
    echo "that contains binary data of a CPIO file containing the input"
    echo "files."
    echo
    exit 1
fi

# Get the absolute path of $1. Unfortunately realpath, which seems the most
# portable option here, only works on existing files.
touch "$1"
OUTPUT_FILE=$(realpath "$1")
rm "$1"

SYMBOL=$2
shift 2

# Determine ".o" file to generate.
case "$PLAT" in
    "integratorcp"|"imx31"|"imx6"|"omap3"|"am335x"|"omap4"|\
    "exynos4"|"exynos5"| "realview")
        FORMAT=elf32-littlearm
        ;;
    "pc99")
        FORMAT=elf32-i386
        ;;
    *)
        echo "$0: Unknown platform \"$PLAT\""
        exit 1
        ;;
esac

# Create working directory.
# Warning: mktemp functions differently on Linux and OSX.
TEMP_DIR=`mktemp -d -t seL4XXXX`
cleanup() {
    rm -rf ${TEMP_DIR}
}
trap cleanup EXIT

# Generate an archive of the input images.
mkdir -p "${TEMP_DIR}/cpio"
for i in $@; do
    cp -f $i "${TEMP_DIR}/cpio"
done
pushd "${TEMP_DIR}/cpio" >/dev/null
ARCHIVE="${TEMP_DIR}/archive.cpio"
ls | cpio -o -H newc > ${ARCHIVE} 2>/dev/null
popd > /dev/null

# Generate a linker script.
LINK_SCRIPT="${TEMP_DIR}/linkscript.ld"
echo "SECTIONS { ._archive_cpio : { ${SYMBOL} = . ; *(.*) ; ${SYMBOL}_end = . ; } }" \
        > ${LINK_SCRIPT}

# Generate an output object file. We switch to the same directory as ${ARCHIVE}
# in order to avoid symbols containing ${TEMP_DIR} polluting the namespace.
pushd "$(dirname ${ARCHIVE})" >/dev/null
${TOOLPREFIX}ld -T ${LINK_SCRIPT} \
        --oformat ${FORMAT} -r -b binary $(basename ${ARCHIVE}) \
        -o ${OUTPUT_FILE}
popd >/dev/null

# Done
exit 0
