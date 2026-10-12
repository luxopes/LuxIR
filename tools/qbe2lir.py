#!/usr/bin/env python3
"""Převod QBE IL (.ssa) na LuxIR (.lir).

Vývojový nástroj: slouží k diferenčním testům proti C QBE a k migraci
existujícího IL. Výstup zachovává pořadí definic, dočasných hodnot,
konstant i bloků, takže LuxIR backend z něj vytvoří stejný assembler,
jaký C QBE vytvoří z původního IL.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = {'w': 'Int32', 'l': 'Int64', 's': 'Float32', 'd': 'Float64'}
SMALL = {'sb': 'Int8', 'ub': 'UInt8', 'sh': 'Int16', 'uh': 'UInt16'}
FIELD = {'b': 'Int8', 'h': 'Int16', 'w': 'Int32', 'l': 'Int64', 's': 'Float32', 'd': 'Float64'}
RESERVED = {
    'function', 'data', 'type', 'export', 'thread', 'common', 'section', 'align',
    'file', 'end', 'goto', 'if', 'else', 'return', 'halt', 'call', 'phi', 'store',
    'blit', 'vastart', 'dbgloc', 'struct', 'union', 'opaque', 'zero', 'extern',
    'Int8', 'UInt8', 'Int16', 'UInt16', 'Int32', 'Int64', 'Float32', 'Float64',
    'Ptr', 'Env', 'inf', 'nan', 'bits', 'ir_version', 'runtime_abi',
}
CMP_INT = {'eq': 'eq', 'ne': 'ne', 'sge': 'ge', 'sgt': 'gt', 'sle': 'le', 'slt': 'lt',
           'uge': 'uge', 'ugt': 'ugt', 'ule': 'ule', 'ult': 'ult'}
CMP_FLT = {'eq': 'eq', 'ne': 'ne', 'ge': 'ge', 'gt': 'gt', 'le': 'le', 'lt': 'lt',
           'o': 'ord', 'uo': 'unord'}
DIRECT = {
    'add', 'sub', 'neg', 'div', 'rem', 'udiv', 'urem', 'mul', 'and', 'or', 'xor',
    'sar', 'shr', 'shl', 'copy', 'alloc1', 'alloc2', 'alloc4', 'alloc8', 'alloc16',
    'vaarg',
}
RENAMED = {
    'extsb': 'sext8', 'extub': 'zext8', 'extsh': 'sext16', 'extuh': 'zext16',
    'extsw': 'sext32', 'extuw': 'zext32', 'exts': 'fext', 'truncd': 'ftrunc',
    'cast': 'bitcast', 'load': 'load', 'loadl': 'load', 'loads': 'load',
    'loadd': 'load', 'loadw': 'load_i32', 'loadsw': 'load_i32', 'loaduw': 'load_u32',
    'loadsb': 'load_i8', 'loadub': 'load_u8', 'loadsh': 'load_i16', 'loaduh': 'load_u16',
}
# operace, jejichž přesná varianta se v LuxIR odvozuje z třídy operandu
INFERRED = {
    'stosi': ('ftoi', 's'), 'dtosi': ('ftoi', 'd'), 'stoui': ('ftou', 's'),
    'dtoui': ('ftou', 'd'), 'swtof': ('itof', 'w'), 'sltof': ('itof', 'l'),
    'uwtof': ('utof', 'w'), 'ultof': ('utof', 'l'),
}
STORES = {'storeb': 'Int8', 'storeh': 'Int16', 'storew': 'Int32', 'storel': 'Int64',
          'stores': 'Float32', 'stored': 'Float64'}
# všechna jména, která LuxIR používá jako klíčová slova nebo instrukce
STORE_CLS = {'storeb': 'w', 'storeh': 'w', 'storew': 'w', 'storel': 'l',
             'stores': 's', 'stored': 'd'}


WORDS = (RESERVED | DIRECT | set(RENAMED.values()) | set(CMP_INT.values())
         | set(CMP_FLT.values()) | {v[0] for v in INFERRED.values()} | {'bits'})


class ConvertError(Exception):
    pass


def load_argcls():
    """Třídy operandů z ops.h: {op: [[cls pro k=w,l,s,d] pro arg0, arg1]}."""
    table = {}
    pattern = re.compile(r'^O\((\w+),\s*T\(([^)]*)\)')
    for line in (ROOT / 'src/ops.h').read_text().splitlines():
        m = pattern.match(line)
        if m:
            cls = [c.strip() for c in m.group(2).split(',')]
            table[m.group(1)] = [cls[0:4], cls[4:8]]
    return table


ARGCLS = load_argcls()
KIDX = {'w': 0, 'l': 1, 's': 2, 'd': 3}

TOKEN = re.compile(r'''
    (?P<ws>[ \t\r]+)
  | (?P<comment>\#[^\n]*)
  | (?P<nl>\n)
  | (?P<flt>[sd]_[-+0-9.a-zA-Z]+)
  | (?P<str>"(?:[^"\\\n]|\\.)*")
  | (?P<glo>\$"(?:[^"\\\n]|\\.)*"|\$[A-Za-z._][A-Za-z0-9_.$]*)
  | (?P<tmp>%[A-Za-z._][A-Za-z0-9_.$]*)
  | (?P<lbl>@[A-Za-z._][A-Za-z0-9_.$]*)
  | (?P<typ>:[A-Za-z._][A-Za-z0-9_.$]*)
  | (?P<int>-?[0-9]+)
  | (?P<dots>\.\.\.)
  | (?P<kw>[A-Za-z._][A-Za-z0-9_.$]*)
  | (?P<punct>[,(){}=+])
''', re.X)


def tokenize(text):
    tokens = []
    line = 1
    pos = 0
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if not m:
            raise ConvertError(f'line {line}: invalid character {text[pos]!r}')
        kind = m.lastgroup
        value = m.group()
        pos = m.end()
        if kind in ('ws', 'comment'):
            continue
        tokens.append((kind, value, line))
        if kind == 'nl':
            line += 1
    tokens.append(('eof', '', line))
    return tokens


def float_text(token):
    text = token[2:]
    if re.fullmatch(r'[-+]?(inf|nan|infinity)', text, re.I):
        return None
    if not re.search(r'[.eE]', text):
        text += '.0'
    if text.startswith('+'):
        text = text[1:]
    if text.startswith('.'):
        text = '0' + text
    if text.startswith('-.'):
        text = '-0' + text[1:]
    if text.endswith('.'):
        text += '0'
    return text


def decode_qbe_string(raw):
    """QBE předává řetězec assembleru beze změny; dekóduje escape GNU as."""
    body = raw[1:-1]
    out = bytearray()
    i = 0
    simple = {'n': 10, 'r': 13, 't': 9, 'b': 8, 'f': 12, '"': 34, '\\': 92}
    while i < len(body):
        c = body[i]
        if c != '\\':
            out += c.encode()
            i += 1
            continue
        i += 1
        c = body[i]
        if c in simple:
            out.append(simple[c])
            i += 1
        elif c in '01234567':
            j = i
            while j < len(body) and j < i + 3 and body[j] in '01234567':
                j += 1
            out.append(int(body[i:j], 8) & 255)
            i = j
        elif c in 'xX':
            j = i + 1
            while j < len(body) and body[j] in '0123456789abcdefABCDEF':
                j += 1
            out.append(int(body[i+1:j], 16) & 255)
            i = j
        else:
            raise ConvertError(f'unsupported string escape \\{c}')
    return bytes(out)


def lir_string(raw):
    data = decode_qbe_string(raw)
    # Přesný text zachováme, pokud ho čtečka LuxIR předá assembleru stejně.
    if re.fullmatch(r'"(?:[^"\\\n]|\\[nrt"\\])*"', raw):
        return raw
    parts = []
    for b in data:
        if b == 10:
            parts.append('\\n')
        elif b == 13:
            parts.append('\\r')
        elif b == 9:
            parts.append('\\t')
        elif b == 34:
            parts.append('\\"')
        elif b == 92:
            parts.append('\\\\')
        elif 32 <= b < 127:
            parts.append(chr(b))
        else:
            parts.append(f'\\x{b:02x}')
    return '"' + ''.join(parts) + '"'


class Parser:
    def __init__(self, tokens):
        self.tokens = tokens
        self.i = 0
        self.out = []
        self.types = {}
        self.type_names = set()

    def peek(self, n=0):
        return self.tokens[self.i + n]

    def next(self):
        tok = self.tokens[self.i]
        self.i += 1
        return tok

    def nextnl(self):
        while self.peek()[0] == 'nl':
            self.i += 1
        return self.next()

    def expect(self, kind, value=None):
        tok = self.next()
        if tok[0] != kind or (value is not None and tok[1] != value):
            raise ConvertError(f'line {tok[2]}: expected {value or kind}, got {tok[1]!r}')
        return tok

    def fail(self, tok, message):
        raise ConvertError(f'line {tok[2]}: {message}')

    def global_name(self, tok):
        if tok[1].startswith('$"'):
            self.fail(tok, 'quoted global names are not supported')
        name = tok[1][1:]
        return name

    def type_name(self, raw):
        name = raw[1:]
        return self.types[name]

    # --- top level ---

    def run(self):
        while True:
            tok = self.nextnl()
            if tok[0] == 'eof':
                break
            linkage = []
            while tok[0] == 'kw' and tok[1] in ('export', 'thread', 'common', 'section'):
                if tok[1] == 'section':
                    sec = self.expect('str')[1]
                    flags = ''
                    if self.peek()[0] == 'str':
                        flags = ' ' + self.next()[1]
                    linkage.append(f'section {sec}{flags}')
                else:
                    linkage.append(tok[1])
                tok = self.nextnl()
            prefix = ' '.join(linkage) + (' ' if linkage else '')
            if tok == ('kw', 'function', tok[2]):
                self.function(prefix)
            elif tok[1] == 'data':
                self.data(prefix)
            elif tok[1] == 'type':
                self.typedef()
            elif tok[1] == 'dbgfile':
                self.out.append(f'file {self.expect("str")[1]}')
            else:
                self.fail(tok, f'unexpected {tok[1]!r}')
        return '\n'.join(self.out) + '\n'

    def typedef(self):
        name_tok = self.nextnl()
        if name_tok[0] != 'typ':
            self.fail(name_tok, 'type name expected')
        qname = name_tok[1][1:]
        name = qname
        if name in WORDS:
            name = 'T_' + name
        while name in self.type_names:
            name += '_'
        self.expect('punct', '=')
        tok = self.nextnl()
        align = ''
        if tok[1] == 'align':
            align = f' align {self.nextnl()[1]}'
            tok = self.nextnl()
        if tok[1] != '{':
            self.fail(tok, '{ expected')
        tok = self.nextnl()
        if tok[0] == 'int':
            size = tok[1]
            if self.nextnl()[1] != '}':
                self.fail(tok, '} expected')
            body = f'opaque({size})'
        elif tok[1] == '{':
            alts = []
            while tok[1] == '{':
                alts.append('struct(' + self.fields(self.nextnl()) + ')')
                tok = self.nextnl()
            if tok[1] != '}':
                self.fail(tok, '} expected')
            body = 'union(' + ', '.join(alts) + ')'
        else:
            body = 'struct(' + self.fields(tok) + ')'
        self.types[qname] = name
        self.type_names.add(name)
        self.out.append(f'type {name}{align} = {body}')

    def fields(self, tok):
        items = []
        while tok[1] != '}':
            if tok[0] == 'typ':
                ftype = self.type_name(tok[1])
            elif tok[1] in FIELD:
                ftype = FIELD[tok[1]]
            else:
                self.fail(tok, f'invalid field {tok[1]!r}')
            tok = self.nextnl()
            if tok[0] == 'int':
                ftype += f' * {tok[1]}'
                tok = self.nextnl()
            items.append(ftype)
            if tok[1] == ',':
                tok = self.nextnl()
            elif tok[1] != '}':
                self.fail(tok, ', or } expected')
        return ', '.join(items)

    def data(self, prefix):
        name = self.global_name(self.nextnl())
        self.expect('punct', '=')
        tok = self.nextnl()
        align = ''
        if tok[1] == 'align':
            align = f' align {self.nextnl()[1]}'
            tok = self.nextnl()
        if tok[1] != '{':
            self.fail(tok, '{ expected')
        items = []
        while True:
            tok = self.nextnl()
            if tok[1] == '}':
                break
            size = tok[1]
            if size not in FIELD and size != 'z':
                self.fail(tok, f'invalid data size {size!r}')
            values = []
            tok = self.nextnl()
            while tok[0] in ('int', 'flt', 'str', 'glo'):
                if tok[0] == 'glo':
                    ref = self.global_name(tok)
                    if self.peek()[1] == '+':
                        self.next()
                        ref += ' + ' + self.expect('int')[1]
                    values.append(('ref', ref))
                elif tok[0] == 'flt':
                    if size not in ('s', 'd'):
                        self.fail(tok, 'floating literal in integer data')
                    text = float_text(tok[1])
                    if text is None:
                        values.append(('val', tok[1][2:].lower()))
                    else:
                        values.append(('val', text))
                elif tok[0] == 'int':
                    if size in ('s', 'd') and int(tok[1]) != 0:
                        values.append(('val', f'bits({tok[1]})'))
                    else:
                        values.append(('val', tok[1]))
                else:
                    values.append(('str', lir_string(tok[1])))
                tok = self.nextnl()
            if size == 'z':
                for kind, value in values:
                    if kind != 'val':
                        self.fail(tok, 'z needs an integer')
                    items.append(f'zero({value})')
            else:
                run = []
                for kind, value in values:
                    if kind == 'str':
                        if run:
                            items.append(f'{FIELD[size]}({", ".join(run)})')
                            run = []
                        if size == 'b':
                            items.append(value)
                        else:
                            items.append(f'{FIELD[size]}({value})')
                    else:
                        if value in ('inf', 'nan', '-inf', '-nan'):
                            value = value.lstrip('-')
                        run.append(value)
                if run:
                    items.append(f'{FIELD[size]}({", ".join(run)})')
            if tok[1] == '}':
                break
            if tok[1] != ',':
                self.fail(tok, ', or } expected')
        if len(items) == 1:
            self.out.append(f'{prefix}data {name}{align} = {items[0]}')
        else:
            self.out.append(f'{prefix}data {name}{align} = [{", ".join(items)}]')

    # --- functions ---

    def cls_spec(self, tok):
        if tok[0] == 'typ':
            return 'agg', self.type_name(tok[1])
        if tok[1] in BASE:
            return tok[1], BASE[tok[1]]
        if tok[1] in SMALL:
            return tok[1], SMALL[tok[1]]
        self.fail(tok, f'invalid class {tok[1]!r}')

    def function(self, prefix):
        tok = self.nextnl()
        ret = ''
        retk = None
        if tok[0] != 'glo':
            code, rtype = self.cls_spec(tok)
            ret = f' -> {rtype}'
            retk = 'l' if code == 'agg' else ('w' if code in SMALL else code)
            tok = self.nextnl()
        name = self.global_name(tok)
        params = self.reflist(param=True)
        if self.nextnl()[1] != '{':
            self.fail(tok, 'function body must start with {')
        body = []
        line = []
        while True:
            tok = self.next()
            if tok[0] == 'eof':
                self.fail(tok, 'unterminated function')
            if tok[0] == 'nl':
                if line:
                    body.append(line)
                    line = []
                continue
            if tok[1] == '}' and not line:
                break
            line.append(tok)
        conv = FunctionConverter(self, name, params, body, retk)
        self.out.append(f'{prefix}function {name}({conv.header()}){ret}:')
        self.out.extend(conv.convert())
        self.out.append('end')
        self.out.append('')

    def reflist(self, param):
        """Seznam parametrů jako tokeny: [(spec, tok)], spec None pro '...'."""
        self.expect('punct', '(')
        items = []
        while self.peek()[1] != ')':
            tok = self.next()
            if tok[0] == 'nl':
                continue
            if tok[0] == 'dots':
                items.append((None, tok))
            elif tok[1] == 'env':
                items.append((('env', 'Env'), self.next()))
            else:
                items.append((self.cls_spec(tok), self.next()))
            if self.peek()[1] == ',':
                self.next()
        self.expect('punct', ')')
        return items


class FunctionConverter:
    def __init__(self, parser, name, params, body, retk):
        self.retk = retk
        self.p = parser
        self.name = name
        self.params = params
        self.body = body
        self.cls = {}
        self.globals = set()
        self.rename = {}

    def fail(self, tok, message):
        self.p.fail(tok, f'in function {self.name}: {message}')

    def scan(self):
        for spec, tok in self.params:
            if spec is not None:
                self.cls[tok[1][1:]] = 'l' if spec[0] in ('env', 'agg') else (
                    'w' if spec[0] in SMALL else spec[0])
        for line in self.body:
            if line[0][0] == 'tmp' and len(line) > 2 and line[1][1] == '=':
                spec = line[2]
                k = 'l' if spec[0] == 'typ' else ('w' if spec[1] in SMALL else spec[1])
                self.cls.setdefault(line[0][1][1:], k)
            for tok in line:
                if tok[0] == 'glo':
                    self.globals.add(tok[1][1:])
        used = set(self.cls) | self.globals | RESERVED
        for name in list(self.cls):
            # globální symboly se rozlišují jen tím, že nejsou definované lokálně
            if name in self.globals or name in ('inf', 'nan'):
                new = name + '_v'
                while new in used:
                    new += '_'
                used.add(new)
                self.rename[name] = new

    def tmp(self, tok):
        name = tok[1][1:]
        if name not in self.cls:
            self.fail(tok, f'temporary %{name} is never defined')
        return self.rename.get(name, name)

    def header(self):
        self.scan()
        parts = []
        for spec, tok in self.params:
            if spec is None:
                parts.append('...')
            else:
                parts.append(f'{spec[1]}({self.tmp(tok)})')
        return ', '.join(parts)

    def operand(self, tok, k, annotate=False):
        """k: třída očekávaná backendem ('w','l','s','d' nebo None)."""
        kind, value = tok[0], tok[1]
        if kind == 'tmp':
            text = self.tmp(tok)
            own = self.cls[value[1:]]
            if annotate and k and own != k:
                return f'{BASE[k]}({text})'
            return text
        if kind == 'glo':
            text = self.p.global_name(tok)
            if annotate and k and k != 'l':
                return f'{BASE[k]}({text})'
            return text
        if kind == 'int':
            if k in ('s', 'd') and int(value) != 0:
                return f'bits({value})'
            if annotate and k:
                return f'{BASE[k]}({value})'
            return value
        if kind == 'flt':
            if k not in ('s', 'd'):
                self.fail(tok, 'floating literal used as an integer is not supported')
            text = float_text(value)
            if text is None:
                text = value[2:].lower().lstrip('+')
                if text.startswith('-'):
                    self.fail(tok, 'negative inf/nan literal is not supported')
                return f'{BASE[k]}({text})'
            if value[0] != k or annotate:
                return f'{BASE[k]}({text})'
            return text
        self.fail(tok, f'operand expected, got {value!r}')

    def split_args(self, tokens):
        """Rozdělí tokeny operandů podle čárek (thread/extern zůstávají s operandem)."""
        args, cur = [], []
        for tok in tokens:
            if tok[1] == ',':
                args.append(cur)
                cur = []
            else:
                cur.append(tok)
        if cur:
            args.append(cur)
        return args

    def ref(self, toks, k, annotate=False):
        if len(toks) == 2 and toks[0][1] in ('thread', 'extern') and toks[1][0] == 'glo':
            return f'{toks[0][1]}({self.p.global_name(toks[1])})'
        if len(toks) != 1:
            self.fail(toks[0], 'unsupported operand syntax')
        return self.operand(toks[0], k, annotate)

    def callargs(self, toks):
        """Argumenty volání v závorkách."""
        if not toks or toks[0][1] != '(' or toks[-1][1] != ')':
            self.fail(toks[0] if toks else ('', '', 0), 'call arguments expected')
        out = []
        for arg in self.split_args(toks[1:-1]):
            if arg[0][0] == 'dots':
                out.append('...')
                continue
            spec = arg[0]
            rest = arg[1:]
            if spec[1] == 'env':
                out.append(f'Env({self.ref(rest, "l")})')
            elif spec[0] == 'typ':
                out.append(f'{self.p.type_name(spec[1])}({self.ref(rest, "l")})')
            elif spec[1] in SMALL:
                out.append(f'{SMALL[spec[1]]}({self.ref(rest, "w")})')
            elif spec[1] in BASE:
                out.append(f'{BASE[spec[1]]}({self.ref(rest, spec[1])})')
            else:
                self.fail(spec, f'invalid argument class {spec[1]!r}')
        return ', '.join(out)

    def convert(self):
        out = []
        for line in self.body:
            out.append(self.line(line))
        return out

    def line(self, line):
        first = line[0]
        if first[0] == 'lbl':
            if len(line) != 1:
                self.fail(first, 'label must be alone on its line')
            return f'{first[1][1:]}:'
        if first[0] == 'tmp':
            if len(line) < 4 or line[1][1] != '=':
                self.fail(first, 'malformed instruction')
            dest = self.tmp(first)
            spec = line[2]
            op = line[3]
            args = line[4:]
            if spec[0] == 'typ':
                dtype = self.p.type_name(spec[1])
                k = 'l'
            elif spec[1] in SMALL:
                dtype = SMALL[spec[1]]
                k = 'w'
            elif spec[1] in BASE:
                dtype = BASE[spec[1]]
                k = spec[1]
            else:
                self.fail(spec, f'invalid class {spec[1]!r}')
            return f'    {dtype}({dest}) = {self.rhs(op, args, k)}'
        return '    ' + self.stmt(first, line[1:])

    def rhs(self, op, args, k):
        name = op[1]
        if name == 'call':
            callee = self.ref(args[:1], 'l')
            return f'call {callee}({self.callargs(args[1:])})'
        if name == 'phi':
            pairs = []
            for arg in self.split_args(args):
                if arg[0][0] != 'lbl':
                    self.fail(arg[0], 'phi label expected')
                pairs.append(f'{arg[0][1][1:]}: {self.ref(arg[1:], k)}')
            return f'phi({", ".join(pairs)})'
        ops = self.split_args(args)
        m = re.fullmatch(r'c(eq|ne|sge|sgt|sle|slt|uge|ugt|ule|ult|ge|gt|le|lt|o|uo)([wlsd])', name)
        if m:
            cond, ok = m.groups()
            table = CMP_INT if ok in 'wl' else CMP_FLT
            if cond not in table:
                self.fail(op, f'invalid comparison {name}')
            a = self.ref(ops[0], ok, annotate=True)
            b = self.ref(ops[1], ok)
            return f'{table[cond]}({a}, {b})'
        if name in INFERRED:
            lname, ok = INFERRED[name]
            return f'{lname}({self.ref(ops[0], ok, annotate=True)})'
        if name in DIRECT or name in RENAMED:
            lname = RENAMED.get(name, name)
            qop = {'loadw': 'loadsw', 'loadl': 'load', 'loads': 'load', 'loadd': 'load',
                   'alloc1': 'alloc4', 'alloc2': 'alloc4'}.get(name, name)
            cls = ARGCLS.get(qop)
            if cls is None:
                self.fail(op, f'unknown operation {name}')
            parts = []
            for n, arg in enumerate(ops):
                ak = cls[n][KIDX[k]] if n < 2 else 'x'
                if ak == 'm':
                    ak = 'l'
                parts.append(self.ref(arg, ak if ak in KIDX else None))
            return f'{lname}({", ".join(parts)})'
        self.fail(op, f'unsupported instruction {name}')

    def stmt(self, first, rest):
        name = first[1]
        if name == 'jmp':
            return f'goto {rest[0][1][1:]}'
        if name == 'jnz':
            args = self.split_args(rest)
            return f'if {self.ref(args[0], "w")} goto {args[1][0][1][1:]} else {args[2][0][1][1:]}'
        if name == 'ret':
            if not rest:
                return 'return'
            if self.retk is None:
                self.fail(first, 'return value in a function without return type')
            return f'return {self.ref(rest, self.retk)}'
        if name == 'hlt':
            return 'halt'
        if name == 'call':
            callee = self.ref(rest[:1], 'l')
            return f'call {callee}({self.callargs(rest[1:])})'
        if name in STORES:
            args = self.split_args(rest)
            k = STORE_CLS[name]
            v = args[0]
            if name in ('storeb', 'storeh'):
                value = f'{STORES[name]}({self.ref(v, "w")})'
            else:
                value = self.ref(v, k, annotate=True)
                if v[0][0] in ('int', 'flt') and '(' not in value:
                    value = f'{STORES[name]}({value})'
            return f'store({value}, {self.ref(args[1], "l")})'
        if name == 'blit':
            args = self.split_args(rest)
            return f'blit({self.ref(args[0], "l")}, {self.ref(args[1], "l")}, {args[2][0][1]})'
        if name == 'vastart':
            return f'vastart({self.ref(rest, "l")})'
        if name == 'dbgloc':
            args = self.split_args(rest)
            return f'dbgloc({", ".join(a[0][1] for a in args)})'
        self.fail(first, f'unsupported statement {name}')

def convert(text):
    parser = Parser(tokenize(text))
    return parser.run()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('input', type=Path)
    ap.add_argument('-o', '--output', type=Path)
    args = ap.parse_args()
    try:
        result = convert(args.input.read_text())
    except ConvertError as e:
        print(f'qbe2lir: {args.input}: {e}', file=sys.stderr)
        return 1
    if args.output:
        args.output.write_text(result)
    else:
        sys.stdout.write(result)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
