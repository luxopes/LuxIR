"""C99/LP64 types for the QBE-to-LSL source port (not a runtime interpreter)."""
from dataclasses import dataclass, field
import ast
import re
from pycparser import c_ast as C


def align_up(n, a):
    return (n + a - 1) // a * a


@dataclass(eq=False)
class Type:
    kind: str
    size: int = 0
    align: int = 1
    signed: bool = False
    name: str = ''
    base: 'Type|None' = None
    count: 'int|None' = None
    fields: list = field(default_factory=list)
    parameters: list = field(default_factory=list)
    variadic: bool = False
    defined: bool = False

    @property
    def packed(self):
        return self.kind == 'struct' and self.name in ('Ref', 'Sym')

    @property
    def aggregate(self):
        return self.kind in ('struct', 'union', 'array') and not self.packed

    @property
    def floating(self):
        return self.kind == 'float'

    @property
    def pointer(self):
        return self.kind in ('pointer', 'array')

    def decay(self):
        return pointer(self.base) if self.kind == 'array' else pointer(self) if self.kind == 'function' else self

    def member(self, name):
        for f in self.fields:
            if f.name == name:
                return f
        raise ValueError(f'no {name} in {self.kind} {self.name}')


@dataclass
class Field:
    name: str
    type: Type
    offset: int
    bit: int = 0
    width: int | None = None


def pointer(base):
    return Type('pointer', 8, 8, base=base)


VOID = Type('void')
INT = Type('int', 4, 4, True, 'int')
UINT = Type('int', 4, 4, False, 'unsigned int')
LONG = Type('int', 8, 8, True, 'long')
ULONG = Type('int', 8, 8, False, 'unsigned long')
DOUBLE = Type('float', 8, 8, True, 'double')
FLOAT = Type('float', 4, 4, True, 'float')
CHAR = Type('int', 1, 1, True, 'char')
BOOL = Type('int', 1, 1, False, '_Bool')


def literal(node):
    if node.type == 'char':
        return ord(ast.literal_eval(node.value))
    if node.type == 'string':
        # Preprocessor may concatenate adjacent strings. Python literal_eval
        # accepts the same adjacent-string form for the ASCII QBE source.
        return ast.literal_eval(node.value)
    if node.type in ('float', 'double'):
        value = re.sub(r'[fFlL]$', '', node.value)
        return float.fromhex(value) if 'p' in value.lower() else float(value)
    text = re.sub(r'[uUlL]+$', '', node.value)
    if re.match(r'^0[0-7]+$', text):
        return int(text, 8)
    return int(text, 0)


class Types:
    def __init__(self):
        self.aliases = {}
        self.tags = {}
        self.constants = {}
        self.variables = {}
        self._seen_enums = set()
        self._building = set()

    def resolve(self, node):
        if isinstance(node, (C.Decl, C.Typedef, C.Typename, C.TypeDecl)):
            return self.resolve(node.type)
        if isinstance(node, C.IdentifierType):
            names = node.names
            if len(names) == 1 and names[0] in self.aliases:
                return self.aliases[names[0]]
            if names == ['void']:
                return VOID
            if names == ['_Bool']:
                return BOOL
            if 'float' in names:
                return FLOAT
            if 'double' in names:
                if 'long' in names:
                    return Type('float', 16, 16, True, 'long double')
                return DOUBLE
            size = 1 if 'char' in names else 2 if 'short' in names else 8 if 'long' in names else 4
            return Type('int', size, size, 'unsigned' not in names, ' '.join(names))
        if isinstance(node, C.PtrDecl):
            return pointer(self.resolve(node.type))
        if isinstance(node, C.ArrayDecl):
            base = self.resolve(node.type)
            count = self.constant(node.dim) if node.dim is not None else None
            return Type('array', base.size*(count or 0), base.align, base=base, count=count)
        if isinstance(node, C.FuncDecl):
            parameters = []
            variadic = False
            for p in (node.args.params if node.args else []):
                if isinstance(p, C.EllipsisParam):
                    variadic = True
                else:
                    t = self.resolve(p)
                    if t.kind != 'void':
                        parameters.append((getattr(p, 'name', None), t.decay()))
            return Type('function', base=self.resolve(node.type), parameters=parameters, variadic=variadic)
        if isinstance(node, C.Enum):
            if node.values is not None and id(node) not in self._seen_enums:
                self._seen_enums.add(id(node))
                value = -1
                for e in node.values.enumerators:
                    value = self.constant(e.value) if e.value is not None else value+1
                    self.constants[e.name] = value
            return INT
        if isinstance(node, (C.Struct, C.Union)):
            kind = 'struct' if isinstance(node, C.Struct) else 'union'
            key = (kind, node.name or f'anon_{id(node)}')
            t = self.tags.setdefault(key, Type(kind, name=node.name or ''))
            if node.decls is None or t.defined or key in self._building:
                return t
            self._building.add(key)
            offset = 0
            bit_unit = None
            bit_used = 0
            max_align = 1
            fields = []
            for decl in node.decls:
                ft = self.resolve(decl)
                max_align = max(max_align, ft.align)
                if decl.bitsize is not None:
                    width = self.constant(decl.bitsize)
                    if kind == 'union':
                        loc, bit = 0, 0
                        offset = max(offset, ft.size)
                    else:
                        if width == 0:
                            offset = align_up(offset, ft.align)
                            bit_unit = None
                            bit_used = 0
                            continue
                        if bit_unit is None or bit_unit[1] != ft.size or bit_used + width > ft.size*8:
                            offset = align_up(offset, ft.align)
                            bit_unit = (offset, ft.size)
                            offset += ft.size
                            bit_used = 0
                        loc, bit = bit_unit[0], bit_used
                        bit_used += width
                    fields.append(Field(decl.name, ft, loc, bit, width))
                else:
                    bit_unit = None
                    bit_used = 0
                    loc = 0 if kind == 'union' else align_up(offset, ft.align)
                    fields.append(Field(decl.name, ft, loc))
                    if kind == 'union':
                        offset = max(offset, ft.size)
                    else:
                        offset = loc + ft.size
            t.fields, t.align = fields, max_align
            t.size, t.defined = align_up(offset, max_align), True
            self._building.remove(key)
            return t
        raise NotImplementedError(f'type {type(node).__name__}: {node}')

    def initialized_type(self, node, init):
        t = self.resolve(node)
        if t.kind == 'array' and t.count is None and init is not None:
            if isinstance(init, C.Constant) and init.type == 'string':
                count = len(literal(init).encode())+1
            elif isinstance(init, C.InitList):
                i = 0
                count = 0
                for entry in init.exprs:
                    if isinstance(entry, C.NamedInitializer):
                        i = self.constant(entry.name[0])
                    count = max(count, i+1)
                    i += 1
            else:
                raise NotImplementedError('incomplete array initializer')
            t.count, t.size = count, count*t.base.size
        return t

    def declaration(self, decl):
        t = self.initialized_type(decl, getattr(decl, 'init', None))
        if isinstance(decl, C.Typedef):
            self.aliases[decl.name] = t
        elif isinstance(decl, C.Decl) and decl.name:
            self.variables[decl.name] = t
        return t

    def expression(self, node):
        if isinstance(node, C.ID):
            return INT if node.name in self.constants else self.variables[node.name]
        if isinstance(node, C.Constant):
            if node.type == 'string':
                return Type('array', len(literal(node).encode())+1, 1, base=CHAR, count=len(literal(node).encode())+1)
            if node.type in ('float', 'double'):
                return FLOAT if node.value.lower().endswith('f') else DOUBLE
            if node.type == 'char':
                return INT
            unsigned = 'unsigned' in node.type or 'u' in node.value.lower()
            value = literal(node)
            size = 8 if 'long' in node.type or value > 4294967295 or (not unsigned and value > 2147483647) else 4
            if value > (1 << (size*8-1))-1:
                unsigned = True
            return Type('int', size, size, not unsigned)
        if isinstance(node, C.Cast):
            return self.resolve(node.to_type)
        if isinstance(node, C.UnaryOp):
            if node.op == 'sizeof':
                return ULONG
            t = self.expression(node.expr)
            if node.op == '&':
                return pointer(t)
            if node.op == '*':
                return t.base
            if node.op == '!':
                return INT
            if node.op in ('+', '-', '~') and t.kind == 'int' and t.size < 4:
                return INT
            return t
        if isinstance(node, C.ArrayRef):
            return self.expression(node.name).base
        if isinstance(node, C.StructRef):
            t = self.expression(node.name)
            if node.type == '->':
                t = t.base
            return t.member(node.field.name).type
        if isinstance(node, C.Assignment):
            return self.expression(node.lvalue)
        if isinstance(node, C.ExprList):
            return self.expression(node.exprs[-1])
        if isinstance(node, C.FuncCall):
            t = self.expression(node.name)
            return (t.base if t.kind == 'pointer' else t).base
        if isinstance(node, C.CompoundLiteral):
            return self.initialized_type(node.type, node.init)
        if isinstance(node, C.TernaryOp):
            a, b = self.expression(node.iftrue), self.expression(node.iffalse)
            if a.kind == 'function':
                return pointer(a)
            if b.kind == 'function':
                return pointer(b)
            if a.pointer:
                return a.decay()
            if b.pointer:
                return b.decay()
            return self.common(a,b)
        if isinstance(node, C.BinaryOp):
            if node.op in ('==','!=','<','>','<=','>=','&&','||'):
                return INT
            a, b = self.expression(node.left), self.expression(node.right)
            if a.pointer:
                return LONG if b.pointer and node.op == '-' else a.decay()
            if b.pointer:
                return b.decay()
            if node.op in ('<<','>>'):
                return INT if a.size < 4 else a
            return self.common(a,b)
        raise NotImplementedError(f'expression type {type(node).__name__}: {node}')

    @staticmethod
    def common(a,b):
        if a.floating or b.floating:
            return DOUBLE if max(a.size,b.size) >= 8 else FLOAT
        a = INT if a.size < 4 else a
        b = INT if b.size < 4 else b
        size = max(a.size,b.size)
        signed = (a.signed if a.size > b.size else b.signed if b.size > a.size else a.signed and b.signed)
        return Type('int',size,size,signed)

    def constant(self, node):
        if isinstance(node, C.Constant):
            return literal(node)
        if isinstance(node, C.ID):
            return self.constants[node.name]
        if isinstance(node, C.Cast):
            value = self.constant(node.expr)
            t = self.resolve(node.to_type)
            if t.kind == 'int':
                value = int(value) & ((1 << (t.size*8))-1)
                if t.signed and value & (1 << (t.size*8-1)):
                    value -= 1 << (t.size*8)
            return value
        if isinstance(node, C.UnaryOp):
            if node.op == 'sizeof':
                t = self.resolve(node.expr) if isinstance(node.expr, C.Typename) else self.expression(node.expr)
                return t.size
            value = self.constant(node.expr)
            return {'+':lambda:+value,'-':lambda:-value,'~':lambda:~value,'!':lambda:int(not value)}[node.op]()
        if isinstance(node, C.BinaryOp):
            a, b = self.constant(node.left), self.constant(node.right)
            return {'+':lambda:a+b,'-':lambda:a-b,'*':lambda:a*b,'/':lambda:a//b,
                    '%':lambda:a%b,'<<':lambda:a<<b,'>>':lambda:a>>b,
                    '&':lambda:a&b,'|':lambda:a|b,'^':lambda:a^b,
                    '==':lambda:int(a==b),'!=':lambda:int(a!=b),'<':lambda:int(a<b),
                    '<=':lambda:int(a<=b),'>':lambda:int(a>b),'>=':lambda:int(a>=b),
                    '&&':lambda:int(bool(a) and bool(b)),'||':lambda:int(bool(a) or bool(b))}[node.op]()
        if isinstance(node, C.TernaryOp):
            return self.constant(node.iftrue if self.constant(node.cond) else node.iffalse)
        raise NotImplementedError(f'constant {type(node).__name__}: {node}')
