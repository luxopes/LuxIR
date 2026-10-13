#!/usr/bin/env python3
"""Generate/check the ABI 1 public scalar IR signatures from runtime headers."""
import argparse,json,subprocess
from pathlib import Path
from pycparser import c_parser,c_ast,c_generator
ROOT=Path(__file__).resolve().parents[1]
HEADERS=sorted((ROOT/'runtime').glob('*.h'))
TARGETS=['amd64_sysv','arm64','rv64']
SCALARS={'void':'Void','uint32_t':'Int32','int32_t':'Int32','uint64_t':'Int64','int64_t':'Int64','uintptr_t':'Int64','luxrt_value':'Int64','int':'Int32'}
def irtype(node):
 if isinstance(node,(c_ast.PtrDecl,c_ast.ArrayDecl)):return 'Ptr'
 if isinstance(node,c_ast.TypeDecl):return irtype(node.type)
 if isinstance(node,c_ast.IdentifierType):
  name=' '.join(node.names)
  if name not in SCALARS:raise ValueError('unmapped public scalar: '+name)
  return SCALARS[name]
 raise ValueError('aggregate public ABI: '+str(node))
def generate():
 source='\n'.join('#include "'+str(p)+'"' for p in HEADERS)
 pp=subprocess.check_output(['cc','-E','-P','-std=c11','-nostdinc','-I',str(ROOT/'tools/runtime-include'),'-x','c','-'],input=source.encode()).decode()
 tree=c_parser.CParser().parse(pp);printer=c_generator.CGenerator();functions=[]
 for decl in tree.ext:
  if not isinstance(decl,c_ast.Decl) or not isinstance(decl.type,c_ast.FuncDecl) or not decl.name.startswith('luxrt_'):continue
  params=[]
  for p in decl.type.args.params if decl.type.args else []:
   kind=irtype(p.type)
   if kind!='Void':params.append({'name':p.name,'ir_type':kind,'c_type':printer.visit(p.type)})
  functions.append({'name':decl.name,'return_ir_type':irtype(decl.type.type),'parameters':params,'targets':TARGETS})
 return {'runtime_abi':'1','guard_symbol':'luxrt_abi_v1_le64_explicit','profile':'Linux64 little-endian, native scalar C ABI, single-owner explicit roots','targets':TARGETS,'calling_convention':'ordinary target C ABI; Value and Float bits use Int64','headers':[str(p.relative_to(ROOT)) for p in HEADERS],'functions':sorted(functions,key=lambda f:f['name']),'layouts':{'heap_bytes_query':'luxrt_heap_bytes','roots_bytes_query':'luxrt_roots_bytes','descriptor_bytes_query':'luxrt_descriptor_bytes','descriptor_fields':['type_id','reference_count','reference_offsets','repeat_offset','count_offset','stride'],'descriptor_field_bytes':8,'root_fields':['previous','slots','count'],'value_tag_bits':3,'small_int_tag':1,'none':2,'false':3,'true':11,'vacant_root':0,'user_type_id_min':256},'compiler_features':['status-invoke','Value/gc_heap automatic shadow roots'],'limitations':['no native PC stack maps','no shared-heap managed threads','no Windows/macOS runtime implementation','OS flags and kernel buffers use documented Linux raw contracts']}
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--check',action='store_true');a=ap.parse_args();p=ROOT/'runtime/abi-v1.json';text=json.dumps(generate(),indent=2)+'\n'
 if a.check:
  if p.read_text()!=text:raise SystemExit('stale runtime/abi-v1.json; public ABI changed')
 else:p.write_text(text)
 print('ABI 1:',len(json.loads(text)['functions']),'public functions')
if __name__=='__main__':main()
