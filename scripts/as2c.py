"""as2c.py: compiles a JavaScript-like subset of ActionScript 2 to Flash 8 action bytecode.

Usage: python scripts/as2c.py <source.as> <out.h> <symbol> [--check]

With --check, writes nothing and fails if <out.h> isn't what <source.as> compiles to.

Writes a C++ header holding the bytecode of a DoAction tag body (ending with
ActionEnd). The program runs as the body of an anonymous function called at
once, so its `var`s are locals; reach globals through _global, _root or plain
names that aren't declared.

Supported: var, function declarations and expressions (closures), if/else,
while, for(;;), break, continue, return; = += -= *= /= ++ --; ?: || && | ^ &
== != === !== < > <= >= instanceof << >> >>> + - * / % ! - ~ typeof delete;
calls, method calls, new, member and index access, array and object literals.
Not supported: switch, try, for..in, do..while, with, <<= >>= and the other
compound assignments. Locals and parameters live in registers unless a nested
function uses them.
"""
import re
import struct
import sys

# --- Lexer -----------------------------------------------------------------

TOKEN = re.compile(r'''
    (?P<ws>\s+|//[^\n]*|/\*.*?\*/)
  | (?P<num>0[xX][0-9a-fA-F]+|\d+\.\d*(?:[eE][-+]?\d+)?|\.\d+(?:[eE][-+]?\d+)?|\d+(?:[eE][-+]?\d+)?)
  | (?P<str>"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')
  | (?P<name>[A-Za-z_$][A-Za-z0-9_$]*)
  | (?P<op>===|!==|>>>|<<=|>>=|\+\+|--|==|!=|<=|>=|&&|\|\||\+=|-=|\*=|/=|<<|>>|[{}()\[\];,.?:=<>+\-*/%!&|^~])
''', re.S | re.X)

KEYWORDS = {'var', 'function', 'if', 'else', 'while', 'for', 'return', 'break', 'continue', 'new',
            'true', 'false', 'null', 'undefined', 'this', 'typeof', 'delete', 'instanceof'}


class Tok:
    def __init__(self, kind, value, line):
        self.kind, self.value, self.line = kind, value, line

    def __repr__(self):
        return f'{self.value!r}@{self.line}'


def lex(src):
    out, pos, line = [], 0, 1
    while pos < len(src):
        m = TOKEN.match(src, pos)
        if not m:
            raise SyntaxError(f'line {line}: unexpected {src[pos:pos + 20]!r}')
        kind = m.lastgroup
        text = m.group()
        if kind == 'num':
            out.append(Tok('num', float(int(text, 16)) if text[:2].lower() == '0x' else float(text), line))
        elif kind == 'str':
            body = text[1:-1]
            body = re.sub(r'\\(.)', lambda e: {'n': '\n', 't': '\t', 'r': '\r'}.get(e.group(1), e.group(1)), body)
            out.append(Tok('str', body, line))
        elif kind == 'name':
            out.append(Tok('kw' if text in KEYWORDS else 'name', text, line))
        elif kind == 'op':
            out.append(Tok('op', text, line))
        line += text.count('\n')
        pos = m.end()
    out.append(Tok('eof', None, line))
    return out


# --- Parser (AST as tuples) --------------------------------------------------

class Parser:
    def __init__(self, toks):
        self.t, self.i = toks, 0

    def peek(self, k=0):
        return self.t[self.i + k]

    def at(self, value, k=0):
        t = self.peek(k)
        return t.kind in ('op', 'kw') and t.value == value

    def eat(self, value=None):
        t = self.t[self.i]
        if value is not None and not (t.kind in ('op', 'kw') and t.value == value):
            raise SyntaxError(f'line {t.line}: expected {value!r}, got {t.value!r}')
        self.i += 1
        return t

    def name(self):
        t = self.eat()
        if t.kind != 'name':
            raise SyntaxError(f'line {t.line}: expected a name, got {t.value!r}')
        return t.value

    def program(self):
        body = []
        while self.peek().kind != 'eof':
            body.append(self.statement())
        return body

    def block(self):
        self.eat('{')
        body = []
        while not self.at('}'):
            body.append(self.statement())
        self.eat('}')
        return ('block', body)

    def statement(self):
        t = self.peek()
        if self.at('{'):
            return self.block()
        if self.at('var'):
            s = self.var_decl()
            self.eat(';')
            return s
        if self.at('function') and self.peek(1).kind == 'name':
            self.eat('function')
            n = self.name()
            return ('var', [(n, self.function_rest(n))])
        if self.at('if'):
            self.eat('if'); self.eat('(')
            c = self.expr(); self.eat(')')
            a = self.statement()
            b = None
            if self.at('else'):
                self.eat('else'); b = self.statement()
            return ('if', c, a, b)
        if self.at('while'):
            self.eat('while'); self.eat('(')
            c = self.expr(); self.eat(')')
            return ('for', None, c, None, self.statement())
        if self.at('for'):
            self.eat('for'); self.eat('(')
            init = None
            if not self.at(';'):
                init = self.var_decl() if self.at('var') else ('expr', self.expr())
            self.eat(';')
            cond = None if self.at(';') else self.expr()
            self.eat(';')
            step = None if self.at(')') else self.expr()
            self.eat(')')
            return ('for', init, cond, step, self.statement())
        if self.at('return'):
            self.eat('return')
            v = None if self.at(';') else self.expr()
            self.eat(';')
            return ('return', v)
        if self.at('break'):
            self.eat('break'); self.eat(';'); return ('break',)
        if self.at('continue'):
            self.eat('continue'); self.eat(';'); return ('continue',)
        if self.at(';'):
            self.eat(';'); return ('block', [])
        e = self.expr()
        self.eat(';')
        return ('expr', e)

    def var_decl(self):
        self.eat('var')
        decls = []
        while True:
            n = self.name()
            v = None
            if self.at('='):
                self.eat('='); v = self.assign()
            decls.append((n, v))
            if not self.at(','):
                return ('var', decls)
            self.eat(',')

    def function_rest(self, name=''):
        self.eat('(')
        params = []
        while not self.at(')'):
            params.append(self.name())
            if self.at(','):
                self.eat(',')
        self.eat(')')
        return ('func', name, params, self.block()[1])

    def expr(self):
        return self.assign()

    def assign(self):
        left = self.ternary()
        for op in ('=', '+=', '-=', '*=', '/='):
            if self.at(op):
                self.eat(op)
                right = self.assign()
                if left[0] not in ('ident', 'member'):
                    raise SyntaxError(f'line {self.peek().line}: bad assignment target')
                if op != '=':
                    right = ('bin', op[0], left, right)
                return ('assign', left, right)
        return left

    def ternary(self):
        c = self.binary(0)
        if self.at('?'):
            self.eat('?')
            a = self.assign(); self.eat(':'); b = self.assign()
            return ('cond', c, a, b)
        return c

    LEVELS = [['||'], ['&&'], ['|'], ['^'], ['&'], ['==', '!=', '===', '!=='],
              ['<', '>', '<=', '>=', 'instanceof'], ['<<', '>>', '>>>'], ['+', '-'], ['*', '/', '%']]

    def binary(self, level):
        if level == len(self.LEVELS):
            return self.unary()
        left = self.binary(level + 1)
        while True:
            for op in self.LEVELS[level]:
                if self.at(op):
                    self.eat(op)
                    right = self.binary(level + 1)
                    left = ('or' if op == '||' else 'and', left, right) if op in ('||', '&&') else ('bin', op, left, right)
                    break
            else:
                return left

    def unary(self):
        for op in ('!', '-', 'typeof', 'delete', '~'):
            if self.at(op):
                self.eat(op)
                return ('unary', op, self.unary())
        for op in ('++', '--'):
            if self.at(op):
                self.eat(op)
                target = self.unary()
                return ('assign', target, ('bin', op[0], target, ('num', 1.0)))
        e = self.postfix()
        for op in ('++', '--'):
            if self.at(op):
                self.eat(op)
                return ('assign', e, ('bin', op[0], e, ('num', 1.0)))
        return e

    def args(self):
        self.eat('(')
        out = []
        while not self.at(')'):
            out.append(self.assign())
            if self.at(','):
                self.eat(',')
        self.eat(')')
        return out

    def postfix(self):
        if self.at('new'):
            self.eat('new')
            callee = self.primary()
            while self.at('.') or self.at('['):
                callee = self.member(callee)
            e = ('new', callee, self.args() if self.at('(') else [])
        else:
            e = self.primary()
        while True:
            if self.at('.') or self.at('['):
                e = self.member(e)
            elif self.at('('):
                e = ('call', e, self.args())
            else:
                return e

    def member(self, obj):
        if self.at('.'):
            self.eat('.')
            t = self.eat()
            if t.kind not in ('name', 'kw'):
                raise SyntaxError(f'line {t.line}: expected a member name')
            return ('member', obj, ('str', t.value))
        self.eat('[')
        k = self.expr()
        self.eat(']')
        return ('member', obj, k)

    def primary(self):
        t = self.eat()
        if t.kind == 'num':
            return ('num', t.value)
        if t.kind == 'str':
            return ('str', t.value)
        if t.kind == 'name':
            return ('ident', t.value)
        if t.kind == 'kw':
            if t.value in ('true', 'false'):
                return ('bool', t.value == 'true')
            if t.value == 'null':
                return ('null',)
            if t.value == 'undefined':
                return ('undef',)
            if t.value == 'this':
                return ('this',)
            if t.value == 'function':
                n = self.name() if self.peek().kind == 'name' else ''
                return self.function_rest(n)
        if t.kind == 'op':
            if t.value == '(':
                e = self.expr(); self.eat(')'); return e
            if t.value == '[':
                items = []
                while not self.at(']'):
                    items.append(self.assign())
                    if self.at(','):
                        self.eat(',')
                self.eat(']')
                return ('array', items)
            if t.value == '{':
                items = []
                while not self.at('}'):
                    k = self.eat()
                    if k.kind not in ('name', 'str', 'kw'):
                        raise SyntaxError(f'line {k.line}: bad object key')
                    self.eat(':')
                    items.append((k.value, self.assign()))
                    if self.at(','):
                        self.eat(',')
                self.eat('}')
                return ('object', items)
        raise SyntaxError(f'line {t.line}: unexpected {t.value!r}')


# --- Scope analysis ----------------------------------------------------------

class FuncInfo:
    def __init__(self, node, parent):
        self.node, self.parent = node, parent
        self.params = list(node[2])
        self.locals = []          # declared with var (in order)
        self.captured = set()     # names a nested function uses
        self.uses_this = False
        self.uses_arguments = False
        self.children = {}


def declared(info):
    return set(info.params) | set(info.locals)


def analyse(node, parent=None):
    """Fills FuncInfo trees: declarations, captures and this/arguments use."""
    info = FuncInfo(node, parent)

    def decls(stmts):
        for s in stmts:
            k = s[0]
            if k == 'var':
                for n, _ in s[1]:
                    if n not in info.locals and n not in info.params:
                        info.locals.append(n)
            elif k == 'block':
                decls(s[1])
            elif k == 'if':
                decls([s[2]] + ([s[3]] if s[3] else []))
            elif k == 'for':
                if s[1] and s[1][0] == 'var':
                    decls([s[1]])
                decls([s[4]])

    decls(node[3])

    def visit(n):
        if not isinstance(n, tuple):
            if isinstance(n, list):
                for x in n:
                    visit(x)
            return
        k = n[0]
        if k == 'func':
            child = analyse(n, info)
            info.children[id(n)] = child
            return
        if k == 'ident':
            if n[1] == 'arguments':
                info.uses_arguments = True
            # A name declared by an enclosing function is captured there.
            f = info
            while f is not None and n[1] not in declared(f):
                f = f.parent
            if f is not None and f is not info:
                f.captured.add(n[1])
            return
        if k == 'this':
            info.uses_this = True
            return
        if k in ('var', 'object'):
            # (name, value) pairs: only the values are code.
            for _, v in n[1]:
                if v is not None:
                    visit(v)
            return
        for x in n[1:]:
            if isinstance(x, (tuple, list)):
                visit(x)

    for s in node[3]:
        visit(s)
    return info


# --- Code generation ---------------------------------------------------------

def push_value(v):
    if v is None:
        return bytes([3])
    if isinstance(v, bool):
        return bytes([5, 1 if v else 0])
    if isinstance(v, str):
        return bytes([0]) + v.encode('utf-8') + b'\0'
    if isinstance(v, float) and v == int(v) and -2 ** 31 <= v < 2 ** 31:
        return bytes([7]) + struct.pack('<i', int(v))
    d = struct.pack('<d', float(v))
    return bytes([6]) + d[4:] + d[:4]


def action(code, payload=b''):
    if code < 0x80:
        return bytes([code])
    return bytes([code]) + struct.pack('<H', len(payload)) + payload


ADD2, SUB, MUL, DIV, MOD = 0x47, 0x0B, 0x0C, 0x0D, 0x3F
BINARY = {'+': [0x47], '-': [0x0B], '*': [0x0C], '/': [0x0D], '%': [0x3F], '==': [0x49], '!=': [0x49, 0x12],
          '===': [0x66], '!==': [0x66, 0x12], '<': [0x48], '>': [0x67], '<=': [0x67, 0x12], '>=': [0x48, 0x12],
          '&': [0x60], '|': [0x61], '^': [0x62], '<<': [0x63], '>>': [0x64], '>>>': [0x65], 'instanceof': [0x54]}


class Code:
    """A function body: actions with symbolic jumps, assembled at the end."""

    def __init__(self):
        self.items = []  # bytes, or ('jump'|'if', label), or ('label', label)
        self.nlabels = 0

    def label(self):
        self.nlabels += 1
        return self.nlabels

    def emit(self, b):
        self.items.append(b)

    def jump(self, kind, label):
        self.items.append((kind, label))

    def place(self, label):
        self.items.append(('label', label))

    def assemble(self):
        pos, labels = 0, {}
        for it in self.items:
            if isinstance(it, tuple) and it[0] == 'label':
                labels[it[1]] = pos
            elif isinstance(it, tuple):
                pos += 5
            else:
                pos += len(it)
        out, pos = bytearray(), 0
        for it in self.items:
            if isinstance(it, tuple) and it[0] == 'label':
                continue
            if isinstance(it, tuple):
                pos += 5
                off = labels[it[1]] - pos
                out += action(0x99 if it[0] == 'jump' else 0x9D, struct.pack('<h', off))
            else:
                out += it
                pos += len(it)
        return bytes(out)


class FuncGen:
    def __init__(self, info):
        self.info = info
        self.code = Code()
        self.regs = {}
        self.loops = []  # (continue label, break label)
        reg = 1
        self.flags = 0x02 | 0x08 | 0x20  # suppress this, arguments, super
        if info.uses_this:
            self.flags = (self.flags & ~0x02) | 0x01
            self.this_reg = reg; reg += 1
        if info.uses_arguments:
            self.flags = (self.flags & ~0x08) | 0x04
            self.args_reg = reg; reg += 1
        self.param_regs = []
        for p in info.params:
            if p in info.captured:
                self.param_regs.append(0)
            else:
                self.regs[p] = reg; self.param_regs.append(reg); reg += 1
        for n in info.locals:
            if n not in info.captured and n not in self.regs:
                self.regs[n] = reg; reg += 1
        self.temp = reg
        self.nregs = reg + 1

    # Values --------------------------------------------------------------
    def push(self, v):
        self.code.emit(action(0x96, push_value(v)))

    def push_reg(self, r):
        self.code.emit(action(0x96, bytes([4, r])))

    def value(self, e):
        k = e[0]
        c = self.code
        if k == 'num':
            self.push(e[1])
        elif k == 'str':
            self.push(e[1])
        elif k == 'bool':
            self.push(e[1])
        elif k == 'null':
            c.emit(action(0x96, bytes([2])))
        elif k == 'undef':
            self.push(None)
        elif k == 'this':
            self.push_reg(self.this_reg)
        elif k == 'ident':
            n = e[1]
            if n == 'arguments' and self.info.uses_arguments:
                self.push_reg(self.args_reg)
            elif n in self.regs:
                self.push_reg(self.regs[n])
            else:
                self.push(n); c.emit(action(0x1C))
        elif k == 'member':
            self.value(e[1]); self.value(e[2]); c.emit(action(0x4E))
        elif k == 'call':
            self.call(e)
        elif k == 'new':
            for a in reversed(e[2]):
                self.value(a)
            self.push(float(len(e[2])))
            callee = e[1]
            if callee[0] == 'member':
                self.value(callee[1]); self.value(callee[2]); c.emit(action(0x53))
            elif callee[0] == 'ident' and callee[1] not in self.regs:
                self.push(callee[1]); c.emit(action(0x40))
            else:
                self.value(callee); self.push(None); c.emit(action(0x53))
        elif k == 'assign':
            self.assign(e[1], e[2], keep=True)
        elif k == 'bin':
            self.value(e[2]); self.value(e[3])
            for op in BINARY[e[1]]:
                c.emit(action(op))
        elif k in ('and', 'or'):
            end = c.label()
            self.value(e[1])
            c.emit(action(0x4C))  # dup
            if k == 'and':
                c.emit(action(0x12))
            c.jump('if', end)
            c.emit(action(0x17))
            self.value(e[2])
            c.place(end)
        elif k == 'cond':
            other, end = c.label(), c.label()
            self.value(e[1]); c.emit(action(0x12)); c.jump('if', other)
            self.value(e[2]); c.jump('jump', end)
            c.place(other); self.value(e[3]); c.place(end)
        elif k == 'unary':
            op = e[1]
            if op == '!':
                self.value(e[2]); c.emit(action(0x12))
            elif op == '-':
                self.push(0.0); self.value(e[2]); c.emit(action(0x0B))
            elif op == '~':
                self.value(e[2]); self.push(-1.0); c.emit(action(0x62))
            elif op == 'typeof':
                self.value(e[2]); c.emit(action(0x44))
            elif op == 'delete':
                t = e[2]
                if t[0] != 'member':
                    raise SyntaxError('delete needs a member')
                self.value(t[1]); self.value(t[2]); c.emit(action(0x3A))
        elif k == 'array':
            for a in reversed(e[1]):
                self.value(a)
            self.push(float(len(e[1]))); c.emit(action(0x42))
        elif k == 'object':
            for n, v in e[1]:
                self.push(n); self.value(v)
            self.push(float(len(e[1]))); c.emit(action(0x43))
        elif k == 'func':
            self.function(e)
        else:
            raise SyntaxError(f'cannot evaluate {k}')

    def call(self, e):
        c = self.code
        callee, args = e[1], e[2]
        for a in reversed(args):
            self.value(a)
        self.push(float(len(args)))
        if callee[0] == 'member':
            self.value(callee[1]); self.value(callee[2]); c.emit(action(0x52))
        elif callee[0] == 'ident' and callee[1] not in self.regs and callee[1] != 'arguments':
            self.push(callee[1]); c.emit(action(0x3D))
        else:
            self.value(callee); self.push(None); c.emit(action(0x52))

    def function(self, node):
        info = self.info.children[id(node)]
        g = FuncGen(info)
        g.body(node[3])
        body = g.code.assemble()
        # Always anonymous: a named DefineFunction2 defines a variable instead of
        # pushing the function, and declarations are assigned like any var.
        header = b'\0' + struct.pack('<HBH', len(node[2]), g.nregs, g.flags)
        for p, r in zip(node[2], g.param_regs):
            header += bytes([r]) + p.encode() + b'\0'
        header += struct.pack('<H', len(body))
        self.code.emit(action(0x8E, header) + body)

    def assign(self, target, value, keep):
        c = self.code
        if target[0] == 'ident':
            n = target[1]
            if n in self.regs:
                self.value(value)
                c.emit(action(0x87, bytes([self.regs[n]])))
                if not keep:
                    c.emit(action(0x17))
            else:
                self.push(n); self.value(value)
                if keep:
                    c.emit(action(0x87, bytes([self.temp])))
                c.emit(action(0x1D))
                if keep:
                    self.push_reg(self.temp)
        else:
            self.value(target[1]); self.value(target[2]); self.value(value)
            if keep:
                c.emit(action(0x87, bytes([self.temp])))
            c.emit(action(0x4F))
            if keep:
                self.push_reg(self.temp)

    # Statements ----------------------------------------------------------
    def body(self, stmts):
        for s in stmts:
            self.stmt(s)

    def stmt(self, s):
        c = self.code
        k = s[0]
        if k == 'block':
            self.body(s[1])
        elif k == 'var':
            for n, v in s[1]:
                if n in self.regs:
                    if v is not None:
                        self.value(v); c.emit(action(0x87, bytes([self.regs[n]]))); c.emit(action(0x17))
                else:
                    self.push(n)
                    if v is not None:
                        self.value(v); c.emit(action(0x3C))
                    else:
                        c.emit(action(0x41))
        elif k == 'expr':
            e = s[1]
            if e[0] == 'assign':
                self.assign(e[1], e[2], keep=False)
            else:
                self.value(e); c.emit(action(0x17))
        elif k == 'if':
            other, end = c.label(), c.label()
            self.value(s[1]); c.emit(action(0x12)); c.jump('if', other)
            self.stmt(s[2])
            if s[3]:
                c.jump('jump', end)
            c.place(other)
            if s[3]:
                self.stmt(s[3])
            c.place(end)
        elif k == 'for':
            init, cond, step, body = s[1], s[2], s[3], s[4]
            if init:
                self.stmt(init)
            top, cont, end = c.label(), c.label(), c.label()
            c.place(top)
            if cond is not None:
                self.value(cond); c.emit(action(0x12)); c.jump('if', end)
            self.loops.append((cont, end))
            self.stmt(body)
            self.loops.pop()
            c.place(cont)
            if step is not None:
                self.stmt(('expr', step))
            c.jump('jump', top)
            c.place(end)
        elif k == 'return':
            if s[1] is None:
                self.push(None)
            else:
                self.value(s[1])
            c.emit(action(0x3E))
        elif k == 'break':
            c.jump('jump', self.loops[-1][1])
        elif k == 'continue':
            c.jump('jump', self.loops[-1][0])
        else:
            raise SyntaxError(f'unknown statement {k}')


def compile_program(src):
    prog = ('func', '', [], Parser(lex(src)).program())
    info = analyse(prog)
    # The program is an anonymous function called at once: push 0 args, the function, undefined.
    outer = FuncGen(FuncInfo(('func', '', [], []), None))
    outer.info.children[id(prog)] = info
    outer.push(0.0)
    outer.function(prog)
    outer.push(None)
    outer.code.emit(action(0x52))
    outer.code.emit(action(0x17))
    return outer.code.assemble() + b'\0'


def main():
    src_path, out_path, symbol = sys.argv[1:4]
    check = sys.argv[4:] == ['--check']
    code = compile_program(open(src_path, encoding='utf-8').read())
    lines = [f'// Generated by scripts/as2c.py from {src_path.replace(chr(92), "/").split("DAORecomp/")[-1]}; do not edit.',
             '#pragma once', '#include <cstdint>', '',
             f'inline constexpr uint8_t {symbol}[] = {{']
    for i in range(0, len(code), 16):
        lines.append('    ' + ', '.join(f'0x{b:02X}' for b in code[i:i + 16]) + ',')
    lines.append('};')
    text = '\n'.join(lines) + '\n'
    if check:
        try:
            current = open(out_path, encoding='utf-8', newline='').read()
        except OSError:
            current = None
        if current != text:
            sys.exit(f'{out_path} is out of date: run python scripts/as2c.py {src_path} {out_path} {symbol}')
        print(f'{out_path}: up to date')
        return
    open(out_path, 'w', newline='\n').write(text)
    print(f'{out_path}: {len(code)} bytes')


if __name__ == '__main__':
    main()
