# RE helper scripts

Requires Python 3 + `pip install capstone`. Run them with a normal `python` (not `-I`) so the user site-packages that hold capstone are found.

**Never name a script `re.py`** — it shadows Python's stdlib `re` module and capstone fails to import with a circular-import error.

## ra3re.py — small x86 helper
```
python ra3re.py <bin> dis <va> [count]   # disassemble N instructions
python ra3re.py <bin> fn <va>            # disassemble until ret/int3
python ra3re.py <bin> refs <va>          # instructions referencing absolute address va
python ra3re.py <bin> calls <va>         # E8/E9 rel32 calls/jumps to va
python ra3re.py <bin> rd <va> [n]        # hex dump initialized bytes (fails for BSS)
python ra3re.py <bin> fstart <va>        # guess function start (walk back to CC CC padding)
```
Addresses are hex VAs (e.g. `0x652880`).

## sweep.py — full linear-sweep disassembly to text
```
python sweep.py <bin> out.asm
```
Writes one `address mnemonic operands` line per instruction for `.text` (~2.9M lines, a few seconds). Grep the output, e.g. `grep "0xcb8514\]" ra3.asm`.

## ctx.py — classify functions that use a pattern
```
python ctx.py ra3.asm <bin> "0xcb8514]"
```
For every function containing the pattern: prints function start, length, call count, the matching instruction context, and any ASCII/UTF-16 strings the function references (source file paths, names) to help tell logic code from client code.
