#!/usr/bin/env python3
"""Generate independently executable LSL from LuxIR's C99 reference sources.

This migration tool resolves C types and memory layouts at generation time.
Generated functions are native LSL functions with direct control flow; there
is no C evaluator, external QBE invocation, or FFI in the generated backend.
Unsupported source constructs fail generation instead of becoming stubs.
"""
from pathlib import Path
from dataclasses import dataclass
from collections import Counter
import argparse
import hashlib
import json
import re
import traceback
from pycparser import c_ast as C
from pycparser.c_generator import CGenerator
from inventory import ROOT, UPSTREAM, sources, parse, VERSION, ARCHIVE_SHA256
from c_types import Type, Types, Field, pointer, INT, UINT, LONG, ULONG, FLOAT, DOUBLE, VOID, literal


# These lowerings depend on the complete pinned C bodies, not just names.
# Hash normalized ASTs (without coordinates) so a source change fails closed.
PINNED_FAST_PATHS = {
    'q_f_alloc': 'ce7ec7436b0ed4ecec7e0b19e0e3b6e2b9daa4b7bcb78735aaba7eac39808edf',
    'q_f_freeall': '5afd2ea9c1954e0af01205a455008e24a4a7fffb7919c2fd162ab26e54d88acc',
    'q_f_bscopy': '24f6540444ad4790fa78cfcf4839976ee360b093684396ea4b3b7e610b85b5ba',
    'q_f_bsunion': '97b489d50c70afecbc21780a3450ac957a4af40c8fcab5164bf63dee133606af',
    'q_f_bsinter': '7ce877854c77b52728a829a2456edb67cb3c1b68de979bec658d9c1d2fb806ca',
    'q_f_bsdiff': '52ae80084296054f4da7c3d2336360ad2ffd24096538edb2a51492628eb05704',
    'q_f_ssa_phiins': 'a8138c7da7592a2a4fb4ac4b90ad956a4503ccc0ee85c995668be6d47ec2bf02',
    'q_f_bsiter': '35321b9fab8da1fdca4a8eafc28eebf578093055ef2a9c70453eca9ce1ee7c5b',
}


def ident(text):
    return re.sub(r'[^a-zA-Z0-9_]', '_', text)


def number(n):
    n = int(n) & ((1 << 64)-1)
    if n >= 1 << 63:
        n -= 1 << 64
    return str(n)


def quote(text):
    return json.dumps(text, ensure_ascii=False)


@dataclass
class Binding:
    name: str
    type: Type
    function: bool = False
    function_id: int = 0
    scalar_local: bool = False


@dataclass
class Place:
    address: str
    type: Type
    bit: int = 0
    width: int | None = None
    packed_base: 'Place|None' = None
    scalar_local: bool = False


class Unit:
    def __init__(self,path):
        self.path = path
        self.key = ident(str(path.relative_to(UPSTREAM).with_suffix('')))
        self.tree = parse(path)
        self.types = Types()
        self.bindings = {}
        for node in self.tree.ext:
            self.types.declaration(node.decl if isinstance(node,C.FuncDef) else node)


class Program:
    def __init__(self):
        self.units = [Unit(p) for p in sources()]
        self.functions = {}
        self.globals = {}
        self.next_function = 1
        self.coverage = []
        self.addresses = set()
        self.strings = {}
        self.readonly_compounds = {}
        self.inline_functions = set()
        self.native_varargs = set()
        self.native_forward_varargs = set()
        self.unique_definition_guards = 0
        self.indexed_definition_loops = 0
        for unit in self.units:
            for node in unit.tree.ext:
                decl = node.decl if isinstance(node,C.FuncDef) else node
                if not isinstance(decl,C.Decl) or decl.name is None:
                    continue
                t = unit.types.variables[decl.name]
                is_fn = t.kind == 'function'
                origin = Path(str(decl.coord.file))
                static = 'static' in decl.storage
                prefix = (ident(str(origin.relative_to(UPSTREAM).with_suffix('')))
                          if origin.is_relative_to(UPSTREAM) else unit.key)
                symbol = ('q_f_' if is_fn else 'q_g_') + ((prefix+'_') if static else '') + decl.name
                if decl.name in unit.bindings:
                    binding = unit.bindings[decl.name]
                else:
                    binding = Binding(symbol,t,is_fn)
                    unit.bindings[decl.name] = binding
                if isinstance(node,C.FuncDef) and symbol not in self.functions:
                    binding.function_id = self.next_function
                    self.next_function += 1
                    self.functions[symbol] = (unit,node,binding)
                elif not is_fn and 'extern' not in decl.storage:
                    if symbol not in self.globals or decl.init is not None:
                        self.globals[symbol] = (unit,decl,binding)
        # Prototypes in other translation units resolve to the same function id.
        for unit in self.units:
            for b in unit.bindings.values():
                if b.function and b.name in self.functions:
                    b.function_id = self.functions[b.name][2].function_id
        calls = Counter()
        def nodes(node):
            yield node
            for _, child in node.children():
                yield from nodes(child)
        generator = CGenerator()
        for name, expected in PINNED_FAST_PATHS.items():
            # __FILE__ in assert/die macros contains the checkout location.
            source = generator.visit(self.functions[name][1]).replace(str(UPSTREAM)+'/', '')
            actual = hashlib.sha256(source.encode()).hexdigest()
            if actual != expected:
                raise RuntimeError(f'pinned QBE fast path body changed: {name}')
        phiins = self.functions['q_f_ssa_phiins'][1]
        loops = [n for n in nodes(phiins) if isinstance(n, C.For)]
        # Only the definition scan uses these initializers in the pinned body.
        def loop_with_init(text):
            matches = [n for n in loops if generator.visit(n.init) == text]
            if len(matches) != 1:
                raise RuntimeError(f'pinned SSA loop not unique: {text}')
            return matches[0]
        self.ssa_block_loop = loop_with_init('b = fn->start')
        self.ssa_instruction_loop = loop_with_init('i = b->ins')
        for unit, node, _ in self.functions.values():
            for item in nodes(node.body):
                if isinstance(item,C.FuncCall) and isinstance(item.name,C.ID):
                    binding = unit.bindings.get(item.name.name)
                    if binding is not None and binding.function:
                        calls[binding.name] += 1
        for symbol, (_, node, binding) in self.functions.items():
            body = list(nodes(node.body))
            # Direct va_arg consumers use an activation-local word vector.
            # Other forwarding wrappers retain the managed API unless audited below.
            if any(isinstance(n,C.FuncCall) and isinstance(n.name,C.ID) and
                   n.name.name == '__qbe_va_arg' for n in body):
                self.native_varargs.add(symbol)
            # Small static leaves with one direct callsite also benefit from
            # expansion (notably cfg.c:inter inside filldom's inner loop).
            # Keep their budget below explicit inline helpers and retain the
            # same scalar-signature/assert-only restrictions for both paths.
            if (('inline' in node.decl.funcspec or
                    ('static' in node.decl.storage and calls[symbol] == 1 and
                     len(body) <= 64)) and
                    len(body) <= 160 and not any(isinstance(n,C.FuncCall) and
                        (not isinstance(n.name,C.ID) or n.name.name != '__qbe_assert')
                        for n in body) and
                    not binding.type.variadic and not binding.type.base.aggregate and
                    not binding.type.base.floating and
                    all(not t.aggregate and not t.floating for _,t in binding.type.parameters)):
                self.inline_functions.add(symbol)
            # The pinned strf wrapper forwards integer/pointer-only format
            # arguments twice. Validate every caller below, and forward the
            # native vector directly instead of allocating/slicing Lists.
            if node.decl.name == 'strf' and Path(node.decl.coord.file).name == 'util.c':
                self.native_forward_varargs.add(symbol)
        self.native_varargs.update(self.native_forward_varargs)
        self.libc = {
            **{n:'mem.'+n for n in ('malloc','calloc','realloc','free','memcpy','memmove','memset','memcmp','strlen','strcmp','strncmp','strcpy','strncpy','strchr')},
            **{n:'stdio.'+n for n in ('fprintf','vfprintf','printf','snprintf','vsnprintf','sprintf','fputs','puts','fputc','putchar','getc','fgetc','ungetc','feof','fopen','fclose','fflush','fscanf','fread')},
            **{n:'libc.c_'+n for n in ('isalpha','isdigit','isspace','isblank','isalnum','isprint','toupper','tolower','strtol','strtoul','strtoll','strtoull','strtod','strtof','atoi','abort','exit')},
            **{n:'libc.c_'+n for n in ('luxir_run','luxir_exists','luxir_exedir','luxir_unlink','luxir_chmod_exe')},
            'abs':'abs','qsort':'q_sort','getopt':'q_getopt','__qbe_assert':'q_assert',
        }

    def native_call(self,t):
        if t.variadic:
            raise NotImplementedError('indirect variadic call')
        count = len(t.parameters) + int(t.base.aggregate)
        if count > 8:
            raise NotImplementedError('indirect call with more than eight word arguments')
        return f'__lsl_qbe_{"fcall" if t.base.floating else "call"}{count}'


class Function:
    def __init__(self,program,unit,node,binding):
        self.p,self.unit,self.node,self.binding = program,unit,node,binding
        self.t = unit.types
        self.vars = dict(unit.bindings)
        self.types_before = dict(self.t.variables)
        self.constants_before = dict(self.t.constants)
        self.lines = []
        self.counter = 0
        self.labels = {}
        self.next_label = 1
        self.block = None
        self.ended = False
        self.return_label = self.label()
        self.breaks = []
        self.continues = []
        self.switches = []
        self.locals = {}
        self.prologue = []
        self.static_initializers = []
        self.generated_nodes = Counter()
        self.return_type = binding.type.base
        self.local_counter = 0
        self.address_taken = set()
        self.inlining = False
        self.inline_counter = 0
        def address_root(n):
            if isinstance(n,C.ID):
                self.address_taken.add(n.name)
            elif isinstance(n,C.StructRef) and n.type == '.':
                address_root(n.name)
        def scan(n):
            if isinstance(n,C.UnaryOp) and n.op == '&':
                address_root(n.expr)
            for _,child in n.children():
                scan(child)
        if node is not None:
            scan(node.body)

    def emit(self,text):
        self.lines.append('    '+text)

    def temporary(self,text):
        self.counter += 1
        name = f'q_v{self.counter}'
        self.emit(name+' = '+text)
        return name

    def label(self,name=None):
        if name in self.labels:
            return self.labels[name]
        result = self.next_label
        self.next_label += 1
        if name is not None:
            self.labels[name] = result
        return result

    def mark(self,label):
        self.emit(f'call __lsl_qbe_label({label})')
        self.block,self.ended = label,False

    def jump(self,label):
        self.emit(f'call __lsl_qbe_jump({label})')
        self.ended = True

    def branch(self,test,yes,no):
        self.emit(f'call __lsl_qbe_branch({test}, {yes}, {no})')
        self.ended = True

    def condition(self,node,yes,no):
        # C short-circuit conditions already describe control flow. Avoid
        # constructing a Boolean join value merely to branch on it again.
        if isinstance(node,C.BinaryOp) and node.op in ('&&','||'):
            right = self.label()
            if node.op == '&&':
                self.condition(node.left,right,no)
            else:
                self.condition(node.left,yes,right)
            self.mark(right)
            self.condition(node.right,yes,no)
        elif isinstance(node,C.UnaryOp) and node.op == '!':
            self.condition(node.expr,no,yes)
        elif isinstance(node,C.TernaryOp):
            left,right = self.label(),self.label()
            self.condition(node.cond,left,right)
            self.mark(left)
            self.condition(node.iftrue,yes,no)
            self.mark(right)
            self.condition(node.iffalse,yes,no)
        else:
            self.branch(self.expr(node),yes,no)

    def local(self,decl):
        ty = self.t.declaration(decl)
        if not decl.name:
            return
        self.local_counter += 1
        name = f'q_s{self.local_counter}_{decl.name}'
        binding = Binding(name,ty)
        if 'static' in decl.storage:
            symbol = self.binding.name+'_'+name
            binding.name = symbol
            self.p.globals[symbol] = (self.unit,decl,binding)
        else:
            binding.scalar_local = not ty.aggregate and decl.name not in self.address_taken
            if binding.scalar_local:
                self.prologue.append(f'    {name} = '+('0.0' if ty.floating else '0'))
            else:
                self.prologue.append(f'    {name} = __lsl_qbe_alloc({max(ty.size,1)})')
        self.vars[decl.name] = binding
        self.locals[decl.name] = binding
        if decl.init is not None and 'static' not in decl.storage:
            self.initialize(Place(name,ty,scalar_local=binding.scalar_local),decl.init)

    def convert(self,value,source,target,force=False):
        if target.kind == 'void':
            return '0'
        if target.floating:
            if source.floating:
                return f'__lsl_qbe_f32({value})' if target.size == 4 and source.size != 4 else value
            result = f'Float({value})'
            if not source.signed and source.size == 8:
                result = f'__lsl_qbe_uitof({value})'
            return f'__lsl_qbe_f32({result})' if target.size == 4 else result
        if source.floating:
            helper = '__lsl_float_to_int' if target.signed else '__lsl_qbe_ftoui'
            value = f'{helper}({value})'
        if target.kind == 'int' and target.size < 8:
            if re.fullmatch(r'-?\d+',value):
                bits = target.size*8
                n = int(value) & ((1 << bits)-1)
                if target.signed and n >= 1 << (bits-1):
                    n -= 1 << bits
                return str(n)
            if not force and source.kind == 'int':
                # Every C-typed operand has already been normalized at its
                # definition/load. Repeating the same extension at each use
                # bloats both LSL AST and pre-SSA QBE without changing bits.
                if source.size == target.size and source.signed == target.signed:
                    return value
                if source.size < target.size and (target.signed or not source.signed):
                    return value
            value = f'__lsl_qbe_{"s" if target.signed else "u"}{target.size*8}({value})'
        return value

    def read(self,place):
        ty = place.type
        if place.scalar_local:
            return place.address
        if place.packed_base is not None:
            value = self.read(place.packed_base)
            bit = place.bit
            width = place.width if place.width is not None else ty.size*8
            return f'(__lsl_qbe_lshr({value}, {bit}) & {number((1<<width)-1)})'
        if ty.aggregate:
            return place.address
        if ty.floating:
            return f'mem.load_float({place.address}, {"True" if ty.size == 4 else "False"})'
        if ty.size not in (1,2,4,8):
            raise ValueError(f'invalid scalar read width {ty.size} for {ty.kind}')
        value = f'__lsl_qbe_load{ty.size*8}({place.address})'
        if place.width is not None:
            value = f'(__lsl_qbe_lshr({value}, {place.bit}) & {number((1<<place.width)-1)})'
            if ty.signed:
                sign = 1 << (place.width-1)
                value = f'(({value} ~ {sign}) - {sign})'
            return value
        return self.convert(value,ULONG,ty)

    def write(self,place,value,source):
        ty = place.type
        if ty.aggregate:
            self.copy(place.address,value,ty.size)
            return value
        value = self.convert(value,source,ty)
        if place.scalar_local:
            self.emit(f'{place.address} = {value}')
            return value
        if place.packed_base is not None:
            base = place.packed_base
            width = place.width if place.width is not None else ty.size*8
            mask = (1 << width)-1
            old = self.read(base)
            combined = f'(({old} & {number(~(mask<<place.bit))}) | __lsl_qbe_shl(({value} & {number(mask)}), {place.bit}))'
            self.write(base,combined,base.type)
            return value
        if place.width is not None:
            mask = (1 << place.width)-1
            old = f'__lsl_qbe_load{ty.size*8}({place.address})'
            value = f'(({old} & {number(~(mask<<place.bit))}) | __lsl_qbe_shl(({value} & {number(mask)}), {place.bit}))'
        if ty.floating:
            self.emit(f'call mem.store_float({place.address}, {value}, {"True" if ty.size == 4 else "False"})')
        else:
            if ty.size not in (1,2,4,8):
                raise ValueError(f'invalid scalar write width {ty.size} for {ty.kind}')
            self.emit(f'call __lsl_qbe_store{ty.size*8}({place.address}, {value})')
        return value

    def copy(self,destination,source,count):
        helper = '__lsl_qbe_copy' if count <= 128 else 'mem.memcpy'
        self.emit(f'call {helper}({destination}, {source}, {count})')

    def place(self,node,force_address=False):
        if isinstance(node,C.ID):
            b = self.vars[node.name]
            return Place(b.name,b.type,scalar_local=b.scalar_local)
        if isinstance(node,C.UnaryOp) and node.op == '*':
            return Place(self.expr(node.expr),self.t.expression(node))
        if isinstance(node,C.ArrayRef):
            base = self.expr(node.name)
            index = self.expr(node.subscript)
            ty = self.t.expression(node)
            return Place(self.temporary(f'({base} + ({index} * {ty.size}))'),ty)
        if isinstance(node,C.StructRef):
            ty = self.t.expression(node.name)
            if node.type == '->':
                base = self.expr(node.name)
                ty = ty.base
                base_place = Place(base,ty)
            else:
                base_place = self.place(node.name,force_address)
            f = ty.member(node.field.name)
            if ty.packed:
                return Place(base_place.address,f.type,f.offset*8+f.bit,f.width,base_place)
            return Place(f'({base_place.address} + {f.offset})',f.type,f.bit,f.width)
        if isinstance(node,C.CompoundLiteral):
            ty = self.t.expression(node)
            if ty.packed and not force_address:
                # Ref/Sym values fit in a word. Building their bit fields in
                # a scalar avoids a memset/helper call for each IR reference.
                # A fresh compound literal cannot alias its own initializer.
                # Build independent fields once, instead of repeatedly mutating
                # the packed word (which introduces unnecessary SSA joins).
                value = self.packed_literal(ty,node.init)
                if value is not None:
                    return Place(self.temporary(value),ty,scalar_local=True)
                place = Place(self.temporary('0'),ty,scalar_local=True)
                self.initialize(place,node.init)
                return place
            # File-scope literals have static storage duration. Their address
            # must remain valid after the initializer function returns.
            if self.node is None:
                address = self.temporary(f'mem.malloc({max(ty.size,1)})')
            else:
                # A syntactic compound literal denotes one automatic object
                # per activation. QBE alloc inside a loop would grow the stack
                # on every evaluation instead of reusing that object's slot.
                address = f'q_compound_{len(self.prologue)}'
                self.prologue.append(f'    {address} = __lsl_qbe_alloc({max(ty.size,1)})')
            place = Place(address,ty)
            self.initialize(place,node.init)
            return place
        raise NotImplementedError(f'lvalue {type(node).__name__} at {node.coord}')

    def expr(self,node):
        self.generated_nodes[type(node).__name__] += 1
        ty = self.t.expression(node)
        if isinstance(node,C.Constant):
            value = literal(node)
            if node.type == 'string':
                # C literals have static duration. Materialize each distinct
                # byte string once rather than allocating an LSL String on
                # every use of an assertion or assembly format string.
                if value not in self.p.strings:
                    self.p.strings[value] = f'q_string_{len(self.p.strings)}'
                return self.p.strings[value]
            if ty.floating:
                v = repr(value)
                return f'__lsl_qbe_f32({v})' if ty.size == 4 else v
            return number(value)
        if isinstance(node,C.ID):
            if node.name in self.t.constants:
                return number(self.t.constants[node.name])
            b = self.vars[node.name]
            if b.function:
                if not b.function_id:
                    raise NotImplementedError('address of external libc function '+node.name)
                self.p.addresses.add(b.function_id)
                return f'__lsl_address({b.name})'
            value = self.read(Place(b.name,b.type,scalar_local=b.scalar_local))
            return value if b.type.aggregate else self.temporary(value)
        if isinstance(node,C.ArrayRef) and isinstance(node.name,C.CompoundLiteral):
            compound = node.name
            array_type = self.t.expression(compound)
            if array_type.kind == 'array' and isinstance(compound.init,C.InitList):
                try:
                    for entry in compound.init.exprs:
                        self.t.constant(entry)
                except (ValueError,TypeError,NotImplementedError,KeyError):
                    pass
                else:
                    # Only an immediate value read is pooled. Mutable lvalues
                    # and address-taking still build the automatic C object.
                    key = id(compound)
                    if key not in self.p.readonly_compounds:
                        name = f'q_const_array_{len(self.p.readonly_compounds)}'
                        self.p.readonly_compounds[key] = name
                        self.p.globals[name] = (self.unit,compound,Binding(name,array_type))
                    base = self.p.readonly_compounds[key]
                    index = self.expr(node.subscript)
                    address = self.temporary(f'({base} + ({index} * {array_type.base.size}))')
                    return self.temporary(self.read(Place(address,array_type.base)))
        if isinstance(node,(C.ArrayRef,C.StructRef,C.CompoundLiteral)):
            value = self.read(self.place(node))
            return value if ty.aggregate else self.temporary(value)
        if isinstance(node,C.Cast):
            v = self.expr(node.expr)
            return self.convert(v,self.t.expression(node.expr),ty)
        if isinstance(node,C.UnaryOp):
            if node.op == 'sizeof':
                return str(self.t.constant(node))
            if node.op == '&':
                if isinstance(node.expr,C.ID) and self.vars[node.expr.name].function:
                    return self.expr(node.expr)
                place = self.place(node.expr,True)
                if place.scalar_local:
                    raise ValueError('address of scalar local missed by escape scan')
                return place.address
            if node.op == '*':
                if ty.kind == 'function':
                    return self.expr(node.expr)
                return self.read(self.place(node))
            if node.op in ('p++','p--','++','--'):
                p = self.place(node.expr)
                old = self.temporary(self.read(p))
                step = ty.base.size if ty.pointer else 1
                v = self.convert(f'({old} {"+" if "+" in node.op else "-"} {step})',ty,ty,force=True)
                new = self.temporary(v)
                self.write(p,new,ty)
                return old if node.op.startswith('p') else new
            v = self.expr(node.expr)
            if node.op == '!':
                return f'Int({v} == 0)'
            if node.op == '~':
                return self.convert(f'({v} ~ -1)',ty,ty,force=True)
            return self.convert(f'({node.op}{v})',ty,ty,force=True)
        if isinstance(node,C.BinaryOp):
            if node.op in ('&&','||'):
                result = self.temporary('0')
                a = self.expr(node.left)
                evaluate,short,done = self.label(),self.label(),self.label()
                self.branch(a,evaluate if node.op=='&&' else short,short if node.op=='&&' else evaluate)
                self.mark(short)
                self.emit(result+' = '+('0' if node.op=='&&' else '1'))
                self.jump(done)
                self.mark(evaluate)
                b = self.expr(node.right)
                self.emit(result+f' = Int({b} != 0)')
                self.jump(done)
                self.mark(done)
                return result
            a,b = self.expr(node.left),self.expr(node.right)
            ta,tb = self.t.expression(node.left),self.t.expression(node.right)
            return self.binary(node.op,a,b,ta,tb)
        if isinstance(node,C.Assignment):
            p = self.place(node.lvalue)
            value = self.expr(node.rvalue)
            source = self.t.expression(node.rvalue)
            if node.op != '=':
                old = self.read(p)
                value = self.binary(node.op[:-1],old,value,p.type,source)
                source = p.type if p.type.pointer else Types.common(p.type,source)
            value = self.temporary(self.convert(value,source,p.type))
            self.write(p,value,p.type)
            return value
        if isinstance(node,C.ExprList):
            value = '0'
            for n in node.exprs:
                value = self.expr(n)
            return value
        if isinstance(node,C.TernaryOp):
            result = self.temporary('0.0' if ty.floating else '0')
            yes,no,done = self.label(),self.label(),self.label()
            self.branch(self.expr(node.cond),yes,no)
            for label,n in ((yes,node.iftrue),(no,node.iffalse)):
                self.mark(label)
                value = self.expr(n)
                self.emit(result+' = '+self.convert(value,self.t.expression(n),ty))
                self.jump(done)
            self.mark(done)
            return result
        if isinstance(node,C.FuncCall):
            value = self.call(node)
            # The C declaration already fixes the return representation. Do
            # not let an LSL Any inference leak through recursive call graphs
            # and turn pointer arithmetic into dynamic object operations.
            if ty.kind != 'void' and not ty.floating:
                return f'__lsl_qbe_raw_int({value})'
            return value
        raise NotImplementedError(f'expression {type(node).__name__} at {node.coord}')

    def binary(self,op,a,b,ta,tb):
        if ta.pointer and op in ('+','-'):
            if tb.pointer:
                return f'__lsl_qbe_idiv(({a} - {b}), {ta.base.size})'
            return f'({a} {op} ({b} * {ta.base.size}))'
        if tb.pointer and op == '+':
            return f'({b} + ({a} * {tb.base.size}))'
        t = ULONG if ta.pointer or tb.pointer else Types.common(ta,tb)
        a,b = self.convert(a,ta,t),self.convert(b,tb,t)
        if t.floating:
            operation = {'+':'fadd','-':'fsub','*':'fmul','/':'fdiv',
                         '==':'feq','!=':'fne','<':'flt','<=':'fle','>':'fgt','>=':'fge'}[op]
            value = f'__lsl_qbe_{operation}({a}, {b})'
            return f'__lsl_qbe_f32({value})' if t.size == 4 and op in ('+','-','*','/') else value
        if op in ('==','!=','<','>','<=','>='):
            if not t.signed and op in ('<','>','<=','>='):
                if op in ('>','>='):
                    a,b = b,a
                return f'__lsl_qbe_{"ult" if op in ("<",">") else "ule"}({a}, {b})'
            return f'Int({a} {op} {b})'
        if op in ('/','%') and not t.floating:
            prefix = 'i' if t.signed else 'u'
            value = f'__lsl_qbe_{prefix}{"div" if op == "/" else "rem"}({a}, {b})'
        elif op in ('<<','>>'):
            shift = 'shl' if op == '<<' else 'ashr' if ta.signed else 'lshr'
            value = f'__lsl_qbe_{shift}({a}, {b})'
        else:
            value = f'({a} {"~" if op == "^" else op} {b})'
        return self.convert(value,t,t,force=True)

    def call(self,node):
        args = list(node.args.exprs if node.args else [])
        name = node.name.name if isinstance(node.name,C.ID) else None
        if name == '__qbe_va_start':
            self.vars[args[0].name] = Binding('q_varargs',Type('varargs'))
            self.t.variables[args[0].name] = Type('varargs')
            self.emit('q_va_index = 0')
            return '0'
        if name == '__qbe_va_end':
            return '0'
        if name == '__qbe_va_arg':
            value = ('__lsl_qbe_load64(q_vararg_words + q_va_index * 8)'
                     if self.binding.name in self.p.native_varargs else
                     'q_varargs[q_va_index]')
            v = self.temporary(value)
            self.emit('q_va_index += 1')
            return f'__lsl_qbe_raw_int({v})'
        ft = self.t.expression(node.name)
        if ft.kind == 'pointer':
            ft = ft.base
        result_buffer = None
        if ft.base.aggregate:
            # C returns an aggregate by value. The receiving activation owns
            # its storage; a pointer into the callee's expired stack is invalid
            # even if an immediate memcpy happens to work on some targets.
            if self.node is None:
                result_buffer = self.temporary(f'mem.malloc({ft.base.size})')
            else:
                result_buffer = f'q_return_{len(self.prologue)}'
                self.prologue.append(f'    {result_buffer} = __lsl_qbe_alloc({ft.base.size})')
        values = []
        for i,arg in enumerate(args):
            if name in ('vfprintf','vsnprintf') and isinstance(arg,C.ID) and self.t.expression(arg).kind == 'varargs':
                if self.binding.name in self.p.native_forward_varargs:
                    values.extend(['q_vararg_words','__lsl_qbe_load64(q_vararg_words - 8)'])
                elif self.binding.name in self.p.native_varargs:
                    raise NotImplementedError('forwarding a consumed native va_list')
                else:
                    # Forward-only wrappers never consume va_arg. Their index
                    # is zero and libc readers do not mutate the List.
                    values.append('q_varargs')
                continue
            at = self.t.expression(arg).decay()
            value = self.expr(arg)
            expected = ft.parameters[i][1] if i < len(ft.parameters) else at
            if name == '__qbe_assert' and i == 0 and (at.floating or at.size > INT.size):
                # assert tests the original C scalar, including all pointer
                # bits; its helper's int parameter is the Boolean result.
                value = f'Int({value} != 0)'
                at = INT
            if i >= len(ft.parameters) and at.floating:
                expected = DOUBLE
            value = self.convert(value,at,expected)
            values.append(self.temporary(value))
        if name in self.p.libc:
            target = self.p.libc[name]
            if name in ('getc', 'fgetc'):
                # Evaluate the FILE argument once. A pure cache hit reads no
                # managed objects; activation and stdin keep full stdio logic.
                cached = self.temporary(f'stdio.getc_ready({values[0]})')
                missing, done = self.label(), self.label()
                self.branch(f'{cached} == -2', missing, done)
                self.mark(missing)
                self.emit(f'{cached} = {target}({values[0]})')
                self.jump(done)
                self.mark(done)
                return cached
            if name in ('vfprintf','vsnprintf') and self.binding.name in self.p.native_forward_varargs:
                target += '_words'
            if name in ('memcpy','memset'):
                try:
                    count = self.t.constant(args[2])
                    zero = name == 'memset' and self.t.constant(args[1]) == 0
                except (ValueError,TypeError,NotImplementedError,KeyError):
                    pass
                else:
                    if 0 <= count <= 128:
                        if name == 'memcpy':
                            return self.temporary(f'__lsl_qbe_copy({values[0]}, {values[1]}, {count})')
                        if zero:
                            return self.temporary(f'__lsl_qbe_zero({values[0]}, {count})')
            if name == '__qbe_assert':
                # Match the C macro: the common successful assertion is a
                # branch, not a helper call + GC poll + exception-flag read.
                # Arguments above have already been evaluated exactly once.
                failed,done = self.label(),self.label()
                self.branch(values[0],done,failed)
                self.mark(failed)
                # This edge is reached only when the already evaluated
                # assertion condition is zero; no Boolean materialization
                # needs to stay live just for the failure diagnostic.
                self.emit('call '+target+'('+', '.join(['0']+values[1:])+')')
                self.jump(done)
                self.mark(done)
                return '0'
            if name in ('fprintf','printf','sprintf','snprintf','fscanf'):
                fixed = {'fprintf':2,'printf':1,'sprintf':2,'snprintf':3,'fscanf':2}[name]
                if (name in ('fprintf','printf') and
                        all(not self.t.expression(a).floating for a in args[fixed:])):
                    count = len(values)-fixed
                    buffer = self.word_arguments(values[fixed:])
                    values = values[:fixed]+[buffer,str(count)]
                    target += '_words'
                else:
                    values = values[:fixed]+['['+', '.join(values[fixed:])+']']
            return self.temporary(target+'('+', '.join(values)+')')
        if name is not None and self.vars[name].function:
            target = self.vars[name].name
            # Ref is a fully packed 32-bit word with no padding. Inline these
            # ubiquitous all.h helpers as word operations, as the C optimizer
            # does, instead of retaining calls and repeated bitfield compares.
            definition = self.p.functions.get(target)
            if (definition is not None and
                    Path(definition[1].coord.file).name == 'all.h' and
                    all(t.packed and t.name == 'Ref' for _,t in ft.parameters)):
                if name == 'req':
                    return self.temporary(f'Int({values[0]} == {values[1]})')
                if name == 'rtype':
                    return self.temporary(f'({values[0]} & 7) - Int({values[0]} == 0)')
                if name == 'rsval':
                    return self.temporary(f'((__lsl_qbe_lshr({values[0]}, 3) & 536870911) ~ 268435456) - 268435456')
            if not self.inlining and target in self.p.inline_functions:
                self.inline_counter += 1
                prefix = f'q_inline_{self.inline_counter}_'
                unit,definition,binding = self.p.functions[target]
                child = Function(self.p,unit,definition,binding)
                child.inlining = True
                child.return_label = self.label()
                child.next_label = self.next_label
                child.generate()
                self.next_label = child.next_label
                result = 'q_result'
                inline_lines = child.lines[:-1]
                # A single terminal return needs no join local or return
                # block in the caller. Preserve the expression itself so a
                # returned comparison remains visible to its consumer.
                def return_count(n):
                    return int(isinstance(n,C.Return)) + sum(return_count(c) for _,c in n.children())
                if (isinstance(definition.body,C.Compound) and definition.body.block_items and
                        isinstance(definition.body.block_items[-1],C.Return) and
                        return_count(definition.body) == 1):
                    positions = [(i,line) for i,line in enumerate(inline_lines)
                                 if line.startswith('    q_result = ')]
                    if positions:
                        position,line = positions[-1]
                        result = line[len('    q_result = '):]
                        inline_lines = inline_lines[:position]
                        child.prologue = [line for line in child.prologue if line != '    q_result = 0']
                local_pattern = r'\bq_(?:v\d+|s\d+_\w+|param\d+|a\d+|result|compound_\d+|return_\d+)\b'
                def rename(line):
                    return re.sub(local_pattern,lambda m:prefix+m[0],line)
                for i,value in enumerate(values):
                    self.emit(f'{prefix}q_a{i} = {value}')
                for line in child.prologue:
                    if ' = __lsl_qbe_alloc(' in line:
                        # One slot per activation/callsite, not a fresh stack
                        # allocation on every iteration of the caller's loop.
                        self.prologue.append(rename(line))
                    else:
                        self.lines.append(rename(line))
                for line in inline_lines:
                    self.lines.append(rename(line))
                self.ended = False
                return rename(result)
            if ft.variadic:
                n = len(ft.parameters)
                if target in self.p.native_varargs:
                    if any(self.t.expression(a).floating for a in args[n:]):
                        raise NotImplementedError('native va_arg floating operand')
                    values = values[:n]+[self.word_arguments(values[n:])]
                else:
                    values = values[:n]+['['+', '.join(values[n:])+']']
            if result_buffer is not None:
                values.insert(0,result_buffer)
            return self.temporary(target+'('+', '.join(values)+')')
        fn = self.expr(node.name)
        target = self.p.native_call(ft)
        return self.temporary(target+'('+', '.join([fn]+([result_buffer] if result_buffer is not None else [])+values)+')')

    def word_arguments(self,values):
        # Each callsite owns one buffer in its activation, reused by loops.
        # The callee consumes the C words synchronously without retaining it.
        if self.node is None:
            raise NotImplementedError('native variadic global initializer')
        buffer = f'q_vararg_{len(self.prologue)}'
        self.prologue.append(f'    {buffer} = __lsl_qbe_alloc({(len(values)+1)*8})')
        self.emit(f'call __lsl_qbe_store64({buffer}, {len(values)})')
        for i,value in enumerate(values):
            self.emit(f'call __lsl_qbe_store64({buffer} + {(i+1)*8}, {value})')
        return f'({buffer} + 8)'

    def packed_literal(self,ty,node):
        if not isinstance(node,C.InitList):
            return None
        # Fall back before emitting any code for nested/designated subobjects.
        if any(isinstance(e,C.InitList) or
               (isinstance(e,C.NamedInitializer) and
                (len(e.name) != 1 or not isinstance(e.name[0],C.ID) or
                 isinstance(e.expr,C.InitList))) for e in node.exprs):
            return None
        values = {}
        index = 0
        for entry in node.exprs:
            expression = entry
            if isinstance(entry,C.NamedInitializer):
                index = next(i for i,f in enumerate(ty.fields) if f.name == entry.name[0].name)
                expression = entry.expr
            field = ty.fields[index]
            value = self.convert(self.expr(expression),self.t.expression(expression),field.type)
            width = field.width if field.width is not None else field.type.size*8
            shift = field.offset*8+field.bit
            mask = (1 << width)-1
            if re.fullmatch(r'-?\d+',value):
                value = number((int(value)&mask) << shift)
            else:
                value = f'({value} & {number(mask)})'
                if shift:
                    value = f'__lsl_qbe_shl({value}, {shift})'
            values[index] = value
            index += 1
        constant = 0
        words = []
        for value in values.values():
            if re.fullmatch(r'-?\d+',value):
                constant |= int(value)
            else:
                words.append(value)
        if constant or not words:
            words.append(number(constant))
        return '('+' | '.join(words)+')'

    def initialize(self,place,node,zero=True):
        ty = place.type
        if isinstance(node,C.InitList):
            if zero:
                if place.scalar_local:
                    self.emit(f'{place.address} = '+('0.0' if ty.floating else '0'))
                else:
                    helper = '__lsl_qbe_zero' if ty.size <= 128 else 'mem.memset'
                    arguments = f'{place.address}, {ty.size}' if ty.size <= 128 else f'{place.address}, 0, {ty.size}'
                    self.emit(f'call {helper}({arguments})')
            index = 0
            for entry in node.exprs:
                path = []
                value = entry
                if isinstance(entry,C.NamedInitializer):
                    path,value = entry.name,entry.expr
                    first = path[0]
                    index = (next(i for i,f in enumerate(ty.fields) if f.name == first.name)
                             if ty.kind in ('struct','union') else self.t.constant(first))
                if ty.kind in ('struct','union'):
                    field = ty.fields[index]
                    target = Place(f'({place.address} + {field.offset})',field.type,field.bit,field.width)
                    if ty.packed:
                        target = Place(place.address,field.type,field.offset*8+field.bit,field.width,place)
                elif ty.kind == 'array':
                    target = Place(f'({place.address} + {index*ty.base.size})',ty.base)
                else:
                    target = place
                for part in path[1:]:
                    if target.type.kind in ('struct','union'):
                        f = target.type.member(part.name)
                        target = Place(f'({target.address} + {f.offset})',f.type,f.bit,f.width)
                    else:
                        i = self.t.constant(part)
                        target = Place(f'({target.address} + {i*target.type.base.size})',target.type.base)
                self.initialize(target,value,False)
                index += 1
            return
        if ty.kind == 'array' and isinstance(node,C.Constant) and node.type == 'string':
            data = literal(node).encode()+b'\0'
            for i,byte in enumerate(data[:ty.count]):
                self.emit(f'call __lsl_qbe_store8({place.address} + {i}, {byte})')
            return
        value = self.expr(node)
        self.write(place,value,self.t.expression(node))

    def stmt(self,node):
        if node is None:
            return
        self.generated_nodes[type(node).__name__] += 1
        if isinstance(node,C.Compound):
            before,types = dict(self.vars),dict(self.t.variables)
            for n in node.block_items or []:
                self.stmt(n)
            self.vars,self.t.variables = before,types
        elif isinstance(node,C.Decl):
            self.local(node)
        elif isinstance(node,C.Typedef):
            self.t.declaration(node)
        elif isinstance(node,C.DeclList):
            for n in node.decls:
                self.local(n)
        elif isinstance(node,C.If):
            yes,no,done = self.label(),self.label(),self.label()
            # A unique definition whose block dominates all ordinary uses,
            # and which is not live on block entry, already needs no SSA phi
            # work. Keep the original path for all other cases (including
            # existing phi operands and loop-carried values).
            unique_guard = (self.binding.name == 'q_f_ssa_phiins' and
                isinstance(node.iftrue,C.Continue) and
                isinstance(node.cond,C.BinaryOp) and node.cond.op == '||' and
                isinstance(node.cond.left,C.ID) and node.cond.left.name == 'ok' and
                isinstance(node.cond.right,C.BinaryOp) and node.cond.right.op == '==' and
                isinstance(node.cond.right.left,C.ID) and node.cond.right.left.name == 'defb')
            if unique_guard:
                check = self.label()
                self.condition(node.cond,yes,check)
                self.mark(check)
                values = [self.read(self.place(C.ID(n))) for n in ('fn','t','defb')]
                self.branch('q_ssa_unique_definition_safe('+', '.join(values)+')',yes,no)
                self.p.unique_definition_guards += 1
            else:
                self.condition(node.cond,yes,no)
            self.mark(yes)
            self.stmt(node.iftrue)
            self.jump(done)
            self.mark(no)
            self.stmt(node.iffalse)
            self.jump(done)
            self.mark(done)
        elif isinstance(node,(C.While,C.DoWhile,C.For)):
            before,types = dict(self.vars),dict(self.t.variables)
            head,body,step,done = self.label(),self.label(),self.label(),self.label()
            indexed_blocks = node is self.p.ssa_block_loop
            if isinstance(node,C.For):
                if indexed_blocks:
                    self.ssa_index_cursor(initial=True)
                    self.p.indexed_definition_loops += 1
                elif node is self.p.ssa_instruction_loop:
                    self.write(self.place(C.ID('i')),
                               '__lsl_qbe_load64(q_definition_node + 16)',
                               self.t.expression(C.ID('i')))
                else:
                    self.stmt(node.init)
            self.jump(body if isinstance(node,C.DoWhile) else head)
            self.mark(head)
            if node.cond is not None:
                self.condition(node.cond,body,done)
            else:
                self.jump(body)
            self.mark(body)
            self.breaks.append(done)
            self.continues.append(head if isinstance(node,C.DoWhile) else step)
            self.stmt(node.stmt)
            self.continues.pop()
            self.breaks.pop()
            self.jump(step)
            self.mark(step)
            if isinstance(node,C.For) and node.next is not None:
                if indexed_blocks:
                    self.ssa_index_cursor(initial=False)
                else:
                    self.expr(node.next)
            self.jump(head)
            self.mark(done)
            self.vars,self.t.variables = before,types
        elif isinstance(node,C.Switch):
            cases = {}
            def collect(n):
                if n is None or isinstance(n,C.Switch):
                    return
                if isinstance(n,(C.Case,C.Default)):
                    cases[id(n)] = (n,self.label())
                for _,child in n.children():
                    collect(child)
            collect(node.stmt)
            done = self.label()
            value = self.temporary(self.expr(node.cond))
            default = done
            for case,label in cases.values():
                if isinstance(case,C.Default):
                    default = label
                else:
                    next = self.label()
                    self.branch(f'({value} == {self.t.constant(case.expr)})',label,next)
                    self.mark(next)
            self.jump(default)
            self.switches.append(cases)
            self.breaks.append(done)
            self.stmt(node.stmt)
            self.breaks.pop()
            self.switches.pop()
            self.mark(done)
        elif isinstance(node,(C.Case,C.Default)):
            self.mark(self.switches[-1][id(node)][1])
            for n in node.stmts or []:
                self.stmt(n)
        elif isinstance(node,C.Break):
            self.jump(self.breaks[-1])
        elif isinstance(node,C.Continue):
            self.jump(self.continues[-1])
        elif isinstance(node,C.Goto):
            self.jump(self.label(node.name))
        elif isinstance(node,C.Label):
            self.mark(self.label(node.name))
            self.stmt(node.stmt)
        elif isinstance(node,C.Return):
            value = self.expr(node.expr) if node.expr is not None else '0'
            source = self.t.expression(node.expr) if node.expr is not None else INT
            if self.return_type.aggregate:
                self.copy('q_return',value,self.return_type.size)
                self.emit('q_result = __lsl_qbe_raw_int(q_return)')
            else:
                self.emit('q_result = '+self.convert(value,source,self.return_type))
            self.jump(self.return_label)
        elif isinstance(node,C.EmptyStatement):
            pass
        elif isinstance(node,(C.FuncCall,C.Assignment,C.UnaryOp,C.ExprList,C.Cast)):
            self.expr(node)
        else:
            raise NotImplementedError(f'statement {type(node).__name__} at {node.coord}')

    def compact_temporaries(self):
        # The C expression walker materializes operands to preserve evaluation
        # order. Constants and aliases need no extra LSL assignment. Only fold
        # temporary-to-temporary aliases within one straight-line basic block:
        # a syntactically single assignment can still execute again in a loop.
        temporary = r'(?:q_inline_\d+_)*q_v\d+'
        token = re.compile(r'\b'+temporary+r'\b')
        assignment = re.compile(r'^    ('+temporary+r') = (.+)$')
        immutable = re.compile(r'(?:-?\d+|q_(?:g_|string_|const_array_)\w+)\Z')
        lines = self.prologue + self.lines
        writes = Counter()
        local_writes = Counter()
        local_assignment = re.compile(r'^    (q_\w+) (?:=|\+=|-=|\*=|//=|%=|&=|\|=|~=|<<=|>>=) ')
        for line in lines:
            match = local_assignment.match(line)
            if match:
                local_writes[match[1]] += 1
        # These definitions execute once per activation in the entry
        # prologue, unlike expression temporaries inside loops. A parameter
        # never reassigned in C and a stack slot's address stay immutable.
        stable_locals = {local_assignment.match(line)[1] for line in self.prologue
                         if local_assignment.match(line) and
                         local_writes[local_assignment.match(line)[1]] == 1}
        blocks = []
        block = 0
        uses = {}
        for index,line in enumerate(lines):
            blocks.append(block)
            match = assignment.fullmatch(line)
            if match:
                writes[match[1]] += 1
                expression = match[2]
            else:
                expression = line
            for name in token.findall(expression):
                uses.setdefault(name,[]).append(index)
            if '__lsl_qbe_label(' in line or '__lsl_qbe_jump(' in line or '__lsl_qbe_branch(' in line:
                block += 1
        aliases = {}
        compact = []
        prologue_size = 0
        for index,line in enumerate(lines):
            match = assignment.fullmatch(line)
            if match and writes[match[1]] == 1:
                name,expression = match.groups()
                expression = aliases.get(expression,expression)
                safe = (bool(immutable.fullmatch(expression)) or expression in stable_locals) and all(
                    i > index for i in uses.get(name,[]))
                if token.fullmatch(expression) and writes[expression] == 1:
                    safe = all(i > index and blocks[i] == blocks[index]
                               for i in uses.get(name,[]))
                if safe:
                    aliases[name] = expression
                    continue
            compact.append(token.sub(lambda m: aliases.get(m[0],m[0]),line))
            if index < len(self.prologue):
                prologue_size += 1
        # Inline expansion reads these lists, so update them as well as the
        # rendered function. Stack allocations remain in the entry prologue.
        self.prologue = compact[:prologue_size]
        self.lines = compact[len(self.prologue):]
        # A predicate consumed immediately by one branch need not become a
        # local first. Keeping its comparison visible lets the frontend emit
        # a native condition directly instead of materialize/test a Boolean.
        # Adjacent statements preserve evaluation order, including effects;
        # do not move definitions across labels, calls or writes.
        uses = Counter()
        for line in self.prologue + self.lines:
            match = assignment.fullmatch(line)
            uses.update(token.findall(match[2] if match else line))
        branch = re.compile(r'^    call __lsl_qbe_branch\((.+), (\d+), (\d+)\)$')
        wrapper = r'(?:(?:__lsl_qbe_(?:raw_int|[su](?:8|16|32))|Int)\()'
        fused = []
        for line in self.lines:
            consumer = branch.fullmatch(line)
            if consumer:
                condition = consumer[1]
                while fused:
                    match = assignment.fullmatch(fused[-1])
                    if not match or uses[match[1]] != 1:
                        break
                    transparent = wrapper+r'*'+re.escape(match[1])+r'\)*'
                    if not re.fullmatch(transparent,condition):
                        break
                    condition = token.sub(lambda m: match[2] if m[0] == match[1] else m[0],condition)
                    fused.pop()
                line = f'    call __lsl_qbe_branch({condition}, {consumer[2]}, {consumer[3]})'
            fused.append(line)
        self.lines = fused

        # Raw CFG terminators end the block. The LSL frontend already skips
        # statements until the next raw label; do not make it parse and retain
        # those dead statements in the first place. A raw label also emits a
        # fallthrough jump itself, so an adjacent jump to that same label is
        # redundant. Keep every label: other blocks may still target it.
        original = len(self.lines)
        live = []
        ended = False
        label = re.compile(r'^    call __lsl_qbe_label\((\d+)\)$')
        terminator = re.compile(r'^    call __lsl_qbe_(?:jump|branch)\(')
        jump = re.compile(r'^    call __lsl_qbe_jump\((\d+)\)$')
        for line in self.lines:
            if label.fullmatch(line):
                ended = False
            elif ended:
                continue
            live.append(line)
            if terminator.match(line):
                ended = True
        compact = []
        for index, line in enumerate(live):
            target = jump.fullmatch(line)
            following = label.fullmatch(live[index+1]) if index+1 < len(live) else None
            if target and following and target[1] == following[1]:
                continue
            compact.append(line)
        self.lines = compact
        self.elided_cfg_statements = original - len(compact)

    def ssa_index_cursor(self, initial):
        """Visit defining blocks in link order, starting at their first def.

        The original scan cannot change anything before that definition or in
        blocks without one. Keep its visit reset over *all* blocks and retain
        the rest of phiins, including instruction renaming and frontier order.
        Earlier temporaries only rewrite their own definitions, so the index
        remains valid when newtmp grows the temporary vector.
        """
        fn = self.read(self.place(C.ID('fn')))
        if initial:
            t = self.read(self.place(C.ID('t')))
            self.emit('if q_definition_index == 0:')
            self.emit(f'    q_definition_index = q_ssa_definition_index({fn})')
            self.emit('end')
            self.emit(f'call q_ssa_clear_block_visits({fn})')
            self.emit(f'q_definition_node = __lsl_qbe_load64(q_definition_index + {t} * 16)')
        else:
            self.emit('q_definition_node = __lsl_qbe_load64(q_definition_node)')
        block = self.place(C.ID('b'))
        self.write(block, '0', block.type)
        self.emit('if q_definition_node != 0:')
        # b is a scalar local in the pinned body.
        self.emit(f'    {block.address} = __lsl_qbe_load64(q_definition_node + 8)')
        self.emit('end')

    def specialize_memory_function(self, original):
        name = self.binding.name
        if name not in {'q_f_alloc', 'q_f_freeall', 'q_f_bscopy', 'q_f_bsunion', 'q_f_bsinter', 'q_f_bsdiff', 'q_f_bsiter'}:
            return original
        header = original[:original.index('\n', original.index('function '))]
        if name == 'q_f_alloc':
            return header + '\n    return mem.function_alloc(q_a0)\nend\n'
        if name == 'q_f_freeall':
            return header + '\n    return mem.function_reset()\nend\n'
        bitset = self.t.aliases['BSet']
        nt = bitset.member('nt').offset
        data = bitset.member('t').offset
        assert bitset.member('nt').type.size == 4 and bitset.member('t').type.base.size == 8
        if name == 'q_f_bsiter':
            # De Bruijn indexing of an isolated low bit replaces firstbit's
            # four dependent branches. The original scan proves word != 0.
            # Reconstruct the 64-byte lookup from the multiplier, not a hand
            # copied table, and keep it in immutable rooted literal storage.
            magic = 0x03f79d71b4cb0a89
            positions = [0] * 64
            for bit in range(64):
                positions[(((1 << bit) * magic) & ((1 << 64)-1)) >> 58] = bit
            # Bias into printable ASCII: LSL literals deliberately accept
            # fewer escape sequences than Python's JSON encoder.
            table = ''.join(chr(bit + 32) for bit in positions)
            if table not in self.p.strings:
                self.p.strings[table] = f'q_string_{len(self.p.strings)}'
            address = self.p.strings[table]
            return header + f'''
    set = __lsl_qbe_raw_int(q_a0)
    element = __lsl_qbe_raw_int(q_a1)
    index = __lsl_qbe_load32(element)
    word_index = __lsl_qbe_lshr(index, 6)
    count = __lsl_qbe_load32(set + {nt})
    if word_index >= count:
        return 0
    end
    data = __lsl_qbe_load64(set + {data})
    word = __lsl_qbe_load64(data + word_index * 8) & __lsl_qbe_shl(-1, index & 63)
    while word == 0:
        word_index += 1
        if word_index >= count:
            return 0
        end
        word = __lsl_qbe_load64(data + word_index * 8)
    end
    lookup = __lsl_qbe_lshr((word & -word) * {magic}, 58)
    bit = __lsl_qbe_load8({address} + lookup) - 32
    call __lsl_qbe_store32(element, word_index * 64 + bit)
    return 1
end
'''
        # Retain the exact original assert diagnostics and word access order.
        assertions = re.findall(r'^    call q_assert\(0, .*\)$', original, re.M)
        if len(assertions) != 1:
            raise RuntimeError(f'pinned bitset assertion missing: {name}')
        body = [header, '    a = __lsl_qbe_raw_int(q_a0)',
                '    b = __lsl_qbe_raw_int(q_a1)',
                f'    count = __lsl_qbe_load32(a + {nt})',
                f'    if count != __lsl_qbe_load32(b + {nt}):',
                '    ' + assertions[0], '    end',
                f'    destination = __lsl_qbe_load64(a + {data})',
                f'    source = __lsl_qbe_load64(b + {data})', '    index = 0']
        def word(i):
            address = f'destination + (index + {i}) * 8'
            left = f'__lsl_qbe_load64({address})'
            right = f'__lsl_qbe_load64(source + (index + {i}) * 8)'
            value = {'q_f_bscopy':right, 'q_f_bsunion':f'{left} | {right}',
                     'q_f_bsinter':f'{left} & {right}',
                     'q_f_bsdiff':f'{left} & (-1 ~ {right})'}[name]
            return f'        call __lsl_qbe_store64({address}, {value})'
        # Raw control-flow edges, as in ordinary translated C, avoid managed
        # loop safepoints caused by the cold assertion's effect annotation.
        body += ['    call __lsl_qbe_label(1)',
                 '    call __lsl_qbe_branch(index + 4 <= count, 2, 3)',
                 '    call __lsl_qbe_label(2)'] + ['    '+word(i).lstrip() for i in range(4)]
        body += ['    index += 4', '    call __lsl_qbe_jump(1)',
                 '    call __lsl_qbe_label(3)',
                 '    call __lsl_qbe_branch(index < count, 4, 5)',
                 '    call __lsl_qbe_label(4)', '    '+word(0).lstrip(),
                 '    index += 1', '    call __lsl_qbe_jump(3)',
                 '    call __lsl_qbe_label(5)', '    return 0', 'end', '']
        return '\n'.join(body)

    def generate(self):
        fn = self.binding.type
        args = [f'q_a{i}' for i,_ in enumerate(fn.parameters)]
        if self.return_type.aggregate:
            args.insert(0,'q_return')
        if fn.variadic:
            args.append('q_varargs')
        self.prologue.append('    q_result = '+('0.0' if self.return_type.floating else '0'))
        if self.binding.name == 'q_f_ssa_phiins':
            self.prologue.append('    q_definition_index = 0')
        if self.binding.name in self.p.native_varargs:
            self.prologue.append('    q_vararg_words = __lsl_qbe_raw_int(q_varargs)')
        for i,(name,ty) in enumerate(fn.parameters):
            local = f'q_param{i}'
            b = Binding(local,ty)
            b.scalar_local = not ty.aggregate and name not in self.address_taken
            if b.scalar_local:
                value = f'q_a{i}' if ty.floating else f'__lsl_qbe_raw_int(q_a{i})'
                self.prologue.append(f'    {local} = '+self.convert(value,ty,ty))
            else:
                self.prologue.append(f'    {local} = __lsl_qbe_alloc({max(ty.size,1)})')
            if name:
                self.vars[name] = b
                self.t.variables[name] = ty
            if b.scalar_local:
                pass
            elif ty.aggregate:
                helper = '__lsl_qbe_copy' if ty.size <= 128 else 'mem.memcpy'
                self.prologue.append(f'    call {helper}({local}, q_a{i}, {ty.size})')
            elif ty.floating:
                self.prologue.append(f'    call mem.store_float({local}, q_a{i}, {"True" if ty.size == 4 else "False"})')
            else:
                self.prologue.append(f'    call __lsl_qbe_store{ty.size*8}({local}, __lsl_qbe_raw_int(q_a{i}))')
        self.mark(self.label())
        self.stmt(self.node.body)
        self.jump(self.return_label)
        self.mark(self.return_label)
        self.emit('return q_result')
        self.t.variables = self.types_before
        self.t.constants = self.constants_before
        self.compact_temporaries()
        origin = str(Path(self.node.coord.file).relative_to(UPSTREAM))
        header = f'# QBE {VERSION}: {origin}:{self.node.coord.line}\nfunction {self.binding.name}('+', '.join(args)+'):'
        return self.specialize_memory_function('\n'.join([header]+self.prologue+self.lines+['end','']))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output',type=Path,default=ROOT/'lsl/luxir.lsl')
    ap.add_argument('--report',type=Path,default=ROOT/'build/translation.json')
    ap.add_argument('--partial',action='store_true',help='diagnostic output only; never marked a complete backend')
    args = ap.parse_args()
    p = Program()
    code = ['# lsl: internal-abi',f'# Generated LSL port of the LuxIR backend (C reference in src/, based on QBE {VERSION}); see src/LICENSE.',
            '# Regenerate: python3 tools/translate.py',
            'load luxir_memory as mem','load luxir_stdio as stdio','load luxir_libc as libc','']
    failures = []
    for name,(unit,node,binding) in p.functions.items():
        try:
            fn = Function(p,unit,node,binding)
            result = fn.generate()
            code.append(result)
            p.coverage.append({'function':name,'source':str(node.coord),'status':'generated',
                               'nodes':dict(fn.generated_nodes),
                               'elided_cfg_statements':fn.elided_cfg_statements})
        except Exception as e:
            failures.append({'function':name,'source':str(node.coord),'error':str(e),'trace':traceback.format_exc()})
    # Every object is allocated before initializers are evaluated, preserving
    # cross-unit C address constants and forward references between tables.
    globals_code = []
    standard = {'stdin':1,'stdout':2,'stderr':3,'optind':1,'opterr':1,'optopt':0,'optarg':0}
    for name,value in standard.items():
        globals_code += [f'q_g_{name} = mem.malloc(8)',f'call __lsl_qbe_store64(q_g_{name}, {value})']
    for name,(unit,decl,binding) in list(p.globals.items()):
        globals_code.append(f'{name} = mem.malloc({max(binding.type.size,1)})')
    for name,(unit,decl,binding) in list(p.globals.items()):
        if decl.init is None:
            continue
        try:
            init_binding = Binding('q_init_'+name,Type('function',base=VOID),True)
            fn = Function(p,unit,None,init_binding)
            fn.initialize(Place(name,binding.type),decl.init)
            code.append('function '+init_binding.name+'():\n'+'\n'.join(fn.lines)+'\nend\n')
            globals_code.append('call '+init_binding.name+'()')
        except Exception as e:
            failures.append({'function':'initializer '+name,'source':str(decl.coord),'error':str(e),'trace':traceback.format_exc()})
    # C function pointers now use real native code addresses and direct
    # indirect calls. No linear function-ID dispatcher remains at runtime.
    comparator = Type('function',base=INT,parameters=[('a',pointer(VOID)),('b',pointer(VOID))])
    comparator_call = p.native_call(comparator)
    support = (Path(__file__).parent/'support.lsl').read_text().replace('QBE_COMPARATOR_DISPATCH',comparator_call)
    # Derive every raw support offset from the same verified LP64 type model.
    ssa_types = p.functions['q_f_ssa_phiins'][0].types
    offsets = {'QBE_FN_TMP':('Fn','tmp'),'QBE_FN_RPO':('Fn','rpo'),
               'QBE_FN_START':('Fn','start'),'QBE_FN_NTMP':('Fn','ntmp'),
               'QBE_BLOCK_INS':('Blk','ins'),'QBE_BLOCK_NINS':('Blk','nins'),
               'QBE_BLOCK_LINK':('Blk','link'),'QBE_BLOCK_VISIT':('Blk','visit'),
               'QBE_INS_TO':('Ins','to'),
               'QBE_BLOCK_IN':('Blk','in'),'QBE_BITSET_DATA':('BSet','t'),
               'QBE_TMP_USES':('Tmp','use'),'QBE_TMP_NUSE':('Tmp','nuse'),
               'QBE_TMP_VISIT':('Tmp','visit'),
               'QBE_BITSET_COUNT':('BSet','nt'),
               'QBE_USE_TYPE':('Use','type'),'QBE_USE_BLOCK':('Use','bid')}
    for token,(tag,field) in offsets.items():
        support = support.replace(token,str(ssa_types.aliases[tag].member(field).offset))
    for token,tag in [('QBE_TMP_SIZE','Tmp'),('QBE_USE_SIZE','Use'),('QBE_INS_SIZE','Ins')]:
        support = support.replace(token,str(ssa_types.aliases[tag].size))
    support = support.replace('QBE_USE_PHI',str(ssa_types.constants['UPhi']))
    support = support.replace('QBE_TMP_FIRST',str(ssa_types.constants['Tmp0']))
    if p.unique_definition_guards != 1:
        raise RuntimeError('pinned QBE unique-definition SSA guard was not matched exactly once')
    if p.indexed_definition_loops != 1:
        raise RuntimeError('pinned QBE SSA definition loop was not matched exactly once')
    code.append(support)
    # __FILE__ in assert messages stays relative so the output is reproducible.
    code += [f'{name} = mem.literal({quote(value.replace(str(UPSTREAM)+"/", "src/"))})' for value,name in p.strings.items()]
    code += globals_code
    code += ['q_argc = arg_count()','q_argv = mem.malloc(q_argc * 8)',
             'for q_index in rang(q_argc):',
             '    call __lsl_qbe_store64(q_argv + q_index * 8, mem.cstr(String(arg(q_index))))',
             'end','call q_f_main(q_argc, q_argv)']
    report = {'version':VERSION,'archive_sha256':ARCHIVE_SHA256,'functions_total':len(p.functions),
              'generated':p.coverage,'failures':failures,'complete':False,
              'semantic_fast_paths':['unique dominating SSA definition without live-in or phi operands',
                                     'PFn zeroed arena with freeall reset',
                                     'ordered unrolled bitset operations',
                                     'SSA definition-block index in original link order',
                                     'bitset iteration with constant-time first set bit',
                                     'pure cached byte input with stdio fallback'],
              'pinned_fast_path_bodies':PINNED_FAST_PATHS,
              'note':'Source generation coverage only; native differential tests are required before completion.'}
    args.report.parent.mkdir(parents=True,exist_ok=True)
    args.report.write_text(json.dumps(report,indent=2)+'\n')
    if failures and not args.partial:
        raise SystemExit(f'{len(failures)} functions blocked; see {args.report}')
    if failures:
        code.insert(1,'# INCOMPLETE DIAGNOSTIC OUTPUT — NOT AN EXECUTABLE BACKEND')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text('\n'.join(code)+'\n')
    print(f'{len(p.coverage)}/{len(p.functions)} functions generated; {len(failures)} blocked')
    for failure in failures:
        print(failure['function']+': '+failure['error'].splitlines()[0])


if __name__ == '__main__':
    main()
