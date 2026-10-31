# zstd decoder

`zstddeclib.c` is the single-file Zstandard decompressor, generated from
facebook/zstd (v1.6.0 development tree, commit 49cf517) with

    cd zstd/build/single_file_libs
    python3 combine.py -r ../../lib -x legacy/zstd_legacy.h -o zstddeclib.c zstddeclib-in.c

Licence: BSD (see LICENSE). Tincture uses it only to read Serum 2
`.SerumPreset` files (Source/Model/SerumPreset.cpp).
