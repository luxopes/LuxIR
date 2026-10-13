#!/usr/bin/env python3
"""Python Float64 oracle: shortest repr, exact midpoints and long decimals."""
import argparse,ctypes,hashlib,json,math,random,struct,subprocess,tempfile
from fractions import Fraction
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
TARGETS=['amd64_sysv','amd64_win','amd64_apple','arm64','arm64_apple','rv64']
def number(bits):return struct.unpack('<d',struct.pack('<Q',bits))[0]
def bits(f):return struct.unpack('<Q',struct.pack('<d',f))[0]
def exact_decimal(value):
 neg=value<0;value=abs(value);a=value.denominator;two=five=0
 while a%2==0:a//=2;two+=1
 while a%5==0:a//=5;five+=1
 assert a==1
 places=max(two,five);s=str(value.numerator*2**(places-two)*5**(places-five))
 if places:s=s.rjust(places+1,'0');s=s[:-places]+'.'+s[-places:]
 return ('-' if neg else '')+s

def corpus():
 rng=random.Random(735913);representations=[0,1,2,(1<<52)-1,1<<52,0x3ff0000000000000,0x3fefffffffffffff,0x7fefffffffffffff,0x7ff0000000000000,0xfff0000000000000,0x7ff8000000000001,0xfff8000000000001,1<<63]
 representations += [rng.getrandbits(64) for _ in range(20000)]
 inputs=['0','-0','+.5','1.','1e+300','1e-999999','-1e999999','inf','-infinity','nan','-nan','0e99999999999999999999']
 for _ in range(5000):
  n=rng.randrange(1,18);digits=str(rng.randrange(1,10))+''.join(str(rng.randrange(10)) for _ in range(n-1));inputs.append(('-' if rng.getrandbits(1) else '')+digits+'e'+str(rng.randrange(-350,309)))
 for _ in range(600):
  n=rng.randrange(18,1201);digits=str(rng.randrange(1,10))+''.join(str(rng.randrange(10)) for _ in range(n-1));inputs.append(('-' if rng.getrandbits(1) else '')+digits+'e'+str(rng.randrange(-323,310)-n))
 edges=[0,1,2,(1<<52)-1,1<<52,0x3fefffffffffffff,0x3ff0000000000000,0x7feffffffffffffe,0x7fefffffffffffff]+[rng.randrange(0,0x7ff0000000000000) for _ in range(100)]
 for b in edges:
  low=Fraction(number(b));upper=Fraction(number(b+1)) if b<0x7fefffffffffffff else low+2**971
  midpoint=(low+upper)/2;text=exact_decimal(midpoint);precision=(len(text.split('.')[1]) if '.' in text else 0)+50;epsilon=Fraction(1,10**precision)
  for x in [midpoint-epsilon,midpoint,midpoint+epsilon]:inputs += [exact_decimal(x),exact_decimal(-x)]
 inputs+=['1.'+'0'*100000,'0.'+'0'*100000+'1e100001']
 return representations,inputs

def verify(lib,reps,inputs):
 parse=lib.luxrt_parse_float64;parse.argtypes=[ctypes.c_void_p,ctypes.c_uint64,ctypes.POINTER(ctypes.c_uint64)];parse.restype=ctypes.c_int32
 fmt=lib.luxrt_format_float64;fmt.argtypes=[ctypes.c_uint64,ctypes.c_void_p,ctypes.c_uint64];fmt.restype=ctypes.c_int64
 checks=0
 for b in reps:
  output=ctypes.create_string_buffer(b'X'*40);n=fmt(b,ctypes.byref(output,4),32);expected=repr(number(b)).encode();assert n==len(expected) and output.raw[4:4+n]==expected,(hex(b),output.raw[4:4+n],expected);checks+=1
  assert output.raw[:4]==b'XXXX' and output.raw[4+n:]==b'X'*(36-n)+b'\x00';checks+=1
  short=ctypes.create_string_buffer(b'X'*40);assert fmt(b,ctypes.byref(short,4),n-1)==-28 and short.raw==b'X'*40+b'\x00';checks+=1
  if not math.isnan(number(b)):
   out=ctypes.c_uint64(42);assert parse(expected,len(expected),ctypes.byref(out))==0 and out.value==b,(hex(b),expected,hex(out.value));checks+=1
 for text in inputs:
  expected=bits(float(text));out=ctypes.c_uint64(42);encoded=text.encode();status=parse(encoded,len(encoded),ctypes.byref(out));assert status==0,(text[:90],status)
  assert out.value==expected or (math.isnan(number(out.value)) and math.isnan(number(expected))),(text[:120],hex(out.value),hex(expected));checks+=1
 for text in ['', ' ','.','+','-','1e','1e+','1..0','0x1p0','1_0','NaN','Infinity','1\x00','1 2','++1','--1']:
  out=ctypes.c_uint64(42);encoded=text.encode();assert parse(encoded,len(encoded),ctypes.byref(out))==-22 and out.value==42;checks+=1
 return checks

def main():
 ap=argparse.ArgumentParser();ap.add_argument('--backend',type=Path,action='append',required=True);ap.add_argument('--report',type=Path);ap.add_argument('--inline',action='store_true');a=ap.parse_args();reps,inputs=corpus();rows=[];assemblies={}
 with tempfile.TemporaryDirectory(prefix='luxrt-float64-') as d:
  d=Path(d);common=[ROOT/'runtime/memory.c',ROOT/'runtime/integer.c'];variants=[('C reference',ROOT/'runtime/float64.c')]
  for i,b in enumerate(a.backend):
   for t in TARGETS:
    asm=subprocess.check_output([str(b.resolve())]+(['-i'] if a.inline else [])+['-t',t,str(ROOT/'runtime/float64.lir')],timeout=30)
    if t in assemblies:assert asm==assemblies[t]
    assemblies[t]=asm
    if t=='amd64_sysv':p=d/('float'+str(i)+'.s');p.write_bytes(asm);variants.append((str(b),p))
  for i,(name,source) in enumerate(variants):
   lib=d/('lib'+str(i)+'.so');subprocess.run(['cc','-O2','-std=c11','-shared','-fPIC','-I'+str(ROOT/'runtime/vendor'),str(source),*map(str,common),'-o',str(lib)],check=True);checks=verify(ctypes.CDLL(str(lib)),reps,inputs);exe=d/('rounding'+str(i));subprocess.run(['cc','-O2',str(ROOT/'tests/runtime/float64_rounding.c'),str(source),*map(str,common),'-I'+str(ROOT/'runtime/vendor'),'-lm','-o',str(exe)],check=True);rounding=subprocess.check_output([str(exe)],timeout=30).decode().strip();rows.append({'implementation':name,'checks':checks,'rounding':rounding});print(name,checks,'checks passed;',rounding,flush=True)
 report={'target':'amd64_sysv','inline':a.inline,'results':rows,'assembly_parity_targets':TARGETS,'oracle':'Python repr/float + Fraction exact midpoint corpus','random_bit_patterns':20000,'long_inputs':600,'midpoint_neighbors':len(inputs)-5614,'huge_input_digits':100000,'libc_float_conversion':False}
 if a.report:a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(report,indent=2)+'\n')
 print(json.dumps(report))
if __name__=='__main__':main()
