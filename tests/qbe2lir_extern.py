#!/usr/bin/env python3
"""External call modifiers survive conversion for result and void calls."""
import argparse,subprocess,tempfile,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
from qbe2lir import convert
SSA='''export function l $probe(l %x) {
@start
 call extern $notify(l %x)
 %y =l call extern $increment(l %x)
 ret %y
}
'''
DRIVER='''#include <stdint.h>
static int64_t seen;
void notify(int64_t x){seen=x;}
int64_t increment(int64_t x){return x+1;}
extern int64_t probe(int64_t);
int main(void){return !(probe(41)==42 && seen==41);}
'''
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);a=ap.parse_args();lir=convert(SSA);assert 'call extern(notify)' in lir and 'call extern(increment)' in lir
 with tempfile.TemporaryDirectory() as d:
  d=Path(d);source=d/'test.lir';source.write_text(lir);driver=d/'driver.c';driver.write_text(DRIVER);old=None
  for b in a.backend:
   asm=subprocess.check_output([str(b.resolve()),str(source)]);assert old is None or asm==old;old=asm;p=d/'test.s';p.write_bytes(asm);exe=d/'test';subprocess.run(['cc',str(driver),str(p),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
 print('extern return-value and void calls: conversion, C/LSL parity and execution passed')
if __name__=='__main__':main()
