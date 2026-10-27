# bm_vanity

A fast search for a Bitmessage address with a word in it, such as `BM-87nickbot...`, and the two keys of every address it finds.
It looks for the kind of address the bots of ynotbit have: version 4, stream 1, a RIPE hash of 20 bytes (one in 256 addresses has a
zero byte first and is a shorter address; those are left out). That kind starts `BM-87`, so the word is looked for from the third
character after `BM-` on: `--pos-min 2` is `BM-87word...`.

```
bm_vanity --word nickbot --pos-min 2 --pos-max 5 -o hits.txt        # any case; Ctrl-C or SIGTERM stops it
bm_vanity --word nickbot --pos-min 2 --pos-max 2 --exact            # only BM-87nickbot..., in lower case
bm_vanity --word nickbot --min-exact 6                              # at least 6 of the 7 letters in lower case
bm_vanity --export BM-87... -f hits.txt -o nickbot.keys             # the two keys of one hit, to a new file
bm_vanity --address-of nickbot.keys                                 # the address of a keys file, made with OpenSSL alone
bm_vanity --selftest                                                # the method against OpenSSL on this machine, and its speed
bm_vanity --word nickbot --no-avx512                                # the hashes of OpenSSL's, where AVX-512 would be used
```

## Build

It is part of the Unix builds (`cmake --build build --target bm_vanity`, tests in `tests/vanity`) and needs OpenSSL and threads
only. To search on a machine, build it there, for that machine's processor:

```
g++ -O3 -march=native -std=c++17 -pthread -Wno-deprecated-declarations -o bm_vanity bm_vanity.cpp avx512.cpp -lcrypto
```

(`vanity.h` has the method, `avx512.cpp` the hashes for processors with AVX-512, `bm_vanity.cpp` the threads and the files.) Run it
with `nice`: it takes every core it is given. On a processor without AVX-512, and in any build for another kind of processor,
`avx512.cpp` is an empty stand-in and OpenSSL's hashes are used; the program says which on its second line.

## How it is fast

A search from a phrase makes two keys from each candidate phrase and pays two point multiplications for them. This one keeps a
random signing key and starts the encryption key at a random number, then steps the encryption key by one: the next public key is the
last one plus G, a single point addition, and 1025 additions share one field inversion (Montgomery's trick). The field arithmetic is
its own, for secp256k1, four 64-bit limbs.

Every point is two keys. The public key of the encryption key `k` is `(x, y)`, and that of the key `n - k` (n the order of the
curve) is `(x, p - y)`: the second key costs a subtraction, not a point, so a step through 1025 points looks at 2050 keys.

An address is not made to look at it either. The address is a number: `04 01`, the 20 bytes of the RIPE and a 4-byte checksum, in base
58. Its first characters depend on its top 128 bits, `04 01` and the first 14 bytes of the RIPE, so one multiplication by a constant
and twelve digit steps give them, and the word is looked for in those digits. What is left per key is the two hashes (SHA-512 of the
130 bytes of the two public keys, then RIPEMD-160), which were OpenSSL's block functions called with nothing around them: 1.1
microseconds a key on a Xeon Gold at 2.2 GHz, two thirds of the time of a search.

On a processor with AVX-512 (`avx512.cpp`) they are done sixteen keys at a time: SHA-512 on the eight 64-bit lanes of a register (twice)
and RIPEMD-160 on sixteen 32-bit lanes, 130 nanoseconds a key. The first characters of the addresses are read off the RIPEs in the
registers too, in floating point, which tells in about 8 nanoseconds a key which keys may have the word (one in a hundred thousand
matches might be lost by it, and the ones it lets through are looked at again with the exact integers). Only those reach the scalar
code. The wide hashes are chosen when the program starts, by what the processor says, and `--no-avx512` turns them off.

On a 28 core Xeon Gold at 2.2 GHz the search from a phrase did 0.48 million keys a second; the first version of this one, with
OpenSSL's hashes, 21 million; with the wide hashes 70 million, and with the second key of every point 99 million (3.6 million on a
single thread). An Apple M-series core does 1.7 million by itself, with no wide hashes.

Only a match is made in full, and made with `namespace ec`, which is OpenSSL's code and shares nothing with the search: its keys
are turned into the address by `EC_POINT_mul`, `SHA512`, `RIPEMD160` and a plain base 58, and the word has to be where the fast way
said. A match that fails that is dropped and counted (a warning at the end); none ever should. The tests check the field against
OpenSSL's big numbers, the points against `EC_POINT_mul`, the digits against the full address of tens of thousands of RIPEs, the
wide hashes against the scalar ones on any limbs (also the filter: it may let a lane through that has no word, never drop one that
has), and a whole walk, with either hashes, against the same keys looked at one by one. The tests of the wide hashes run where the
processor has AVX-512 and are skipped elsewhere.

## What to expect

A word of `n` letters at one place comes up once in about `58^n / spellings` addresses. `nickbot` has 7 letters (58^7 is 2.2e12), and
its `i` and `o` have no capital in base 58, so it has 32 spellings. Right after `BM-87` it comes up 2.5 times as often, because the
character there can only be one of 24 (`S` to `q`); but `N` is not one of them, so only `BM-87n...`, with 16 spellings, can be.
The exact odds per address:

| | any case | exact lower case |
|---|---|---|
| right after `BM-87` (`--pos-min 2 --pos-max 2`) | 1 in 5.5e10 | 1 in 8.9e11 |
| one to three characters later | 1 in 7e10 each | 1 in 2.2e12 each |
| any of the four places | 1 in 1.6e10 | 1 in 4.0e11 |

At 99 million keys a second (the Xeon above) that is a hit about every 3 minutes for any case at any place; `BM-87nickbot` in any
case every 9 minutes, in lower case every 2.5 hours (every 68 minutes at any of the four places). A search has no memory: it is never
"due".

## Secrecy

A line of the hits file is `address place letters-as-typed signing-key encryption-key`: the keys are the address. The file is made
with mode 600, nothing but addresses is printed, and `--export` makes its file with mode 600 and never over one that is there. Keep it
that way: move a keys file between machines with `ssh ... | ssh ...` and `umask 077`, not by chat, mail or a repository.

## After a search

`--export` makes a keys file: the signing key and then the encryption key, one to a line, in hex. That is what the `keys_file` of
`bmbot_open` takes (`bmbot/bmbot.h`), and what `nickbot --keys-file` takes (`docs/nickbot.md`). `--address-of` prints the address of
a keys file, and the bot prints the same on its first line; compare the two before anything is announced. The keys file is the only
copy of the address: back it up.
