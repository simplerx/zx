# Zx

A minimal systems programming language.

- **One value, one type**: `?T` and `!T` are ordinary value types, with exactly
  four ways out — `try`, catch, `orelse`, and `switch`. No guessing, no hidden paths.
- **One concurrency abstraction**: the `std.task.Task` instance. Threads,
  coroutines, task pools, select, and default channels all hang off the instance;
  `std.io` stays purely synchronous — for concurrency you compose `task + io`
  explicitly.
- **One memory model**: manual management. The standard library provides an
  arena and a pool; there are no implicit allocations.
- **C all the way down**: the backend compiles Zx to C, then hands it to the
  bundled zig `cc`. The generated code is readable, and `--emit-c` prints it —
  performance problems never hide inside a black box.

Self-hosted: the frontend (lexer/parser) and the entire standard library are
written in Zx itself; the compiler driver is C.

## Language in one glance

```zx
const std = @mod("std");

const Point = struct {
    x: f64,
    y: f64,

    const len2 = fn (self: *Self) f64 {
        return self.x * self.x + self.y * self.y;
    };
};

const parse = fn (s: []const u8) !Point {
    let n = std.str.parseInt(s);          // ?i64
    if (n) v {
        return Point{ .x = (f64)v, .y = 0.0 };
    }
    return error.bad_number;
};

pub const main = fn () !i32 {
    var p = Point{ .x = 3.0, .y = 4.0 };
    std.println("{}", p.len2());          // 25

    if (parse("12")) pt {                 // catch: value on success, error code on failure
        std.println("x={}", pt.x);
    } else e {
        std.println("err {}", std.debug.errName(e));
    }
    return 0;
};
```

Key points: variables are either `let` (immutable) or `var` (mutable); functions
are values, written as `const name = fn (args) ret { ... };`; `!T` means
"either a `T` or an error"; `Self` refers to the enclosing struct; there is no
`break` — early exit uses labeled expressions:
`const v = done: while (...) { ... done: 42; };`.

## Build and test

```sh
sh build.sh     # -> bin/zxc.exe (compiler), bin/zxfe.exe (bootstrapped frontend), bin/zxls.exe (language server)
sh test.sh      # full regression: syntax / semantics / negative cases / runtime / multi-file / incremental / CLI / LSP
sh tools/stress.sh  # stress suite: 3 regression rounds / fuzz / LSP abuse / 600k numeric samples / concurrent builds
```

The toolchain ships inside the repo (`.toolchain/zig.exe`, not tracked, ~360 MB).
If it is missing, unpack zig 0.17.0 to `.toolchain/zig.exe`, or run
`ZIG=zig sh build.sh` to use a system zig. Windows is the primary platform
(the FFI demos and the language server are written against it); other
platforms are unverified.

A demo with built-in self-checks:

```sh
bin/zxc.exe demo/snake.zx -o snake.exe
./snake.exe          # console snake: WASD/arrows, P pause, R restart, Q quit
./snake.exe --sim    # unattended: a bot plays 300 games, verifying invariants at every step
```

## Commands

Single file:

```sh
zxc file.zx              # static check (default)
zxc file.zx -o prog      # produce an executable
zxc file.zx --emit-c     # print the generated C only
zxc file.zx --ast        # print the syntax tree
zxc file.zx --tokens     # print tokens
zxc file.zx --release    # -O2 (dev default is -O0)
```

Projects (reads `zx.json` in the current directory):

```sh
zxc init [name]          # scaffold: src/ + vendor/ + zx.json
zxc run                  # build and run
zxc build                # artifacts go to out/, intermediates to .zxc-cache/
zxc test [filter]        # run cases in src/test/ (zero-arg pub functions)
zxc check                # static-check the whole project
zxc fmt [--check]        # format
```

Binding C libraries:

```sh
zxc include ../raylib    # directory -> vendor/<name>.zx bindings + vendor/<name>.clib manifest
zxc include foo.h        # a single header
zxc add https://.../xx.h  or xx.git  or lib  # fetch from the network into vendor/, SHA-256 / commit recorded in zx.lock
zxc fetch                # reproduce vendor/ from the lock file on a new machine
```

`vendor/` is a module search root: after `const r = @mod("raylib");`, completion,
go-to-definition, and inlay type hints all work as usual. The generated bindings
are ordinary Zx source files you can edit freely.

## Performance

Compared against `zig cc -O2` on the same baselines (`sh bench/run.sh`,
median of 7 runs):

| Workload | Zx | Hand-written C |
| --- | --- | --- |
| fib(34) | 14.4 ms | 14.1 ms |
| mandel | 130.9 ms | 132.0 ms |
| matrix multiply | 92.1 ms | 93.6 ms |
| string scan | 2.6 ms | 2.0 ms |

**Runtime performance is on par with hand-written C**: what gets generated is
plain C — no runtime interpretation, no implicit boxing, no GC pauses.
Containers (`ArrayList` / `HashMap`) use `@memcpy`/`@memmove`/`@memset` bulk
paths and cache the hash with its key on growth; measured ~23% faster than a
naive implementation.

The compile side is linear too: small files build in ~0.1 s, multi-file
projects rebuild incrementally per-module `.o`; the language server computes
48,000 inlay type hints for a 16,000-line file in 0.09 s.

## Layout

| Path | Contents |
| --- | --- |
| `src/` | the compiler in C: lexer / parser / semantic analysis / C emission / diagnostics |
| `bootstrap/` | bootstrapped frontend (written in Zx), built as `bin/zxfe.exe`, used by default |
| `std/` | standard library: `str` `map` `fs` `json` `time` `arena` `pool` `task` |
| `tests/` | `good/` positive cases, `bad/` negative cases (with expected error codes), `multi/` multi-file |
| `demo/` | `snake.zx` console game; `ffi/` Win32 + OpenGL binding demos |
| `bench/` | benchmarks against hand-written C, plus baselines |
| `spec/` | `zx-spec.md` the spec, `zx.ebnf` the grammar, `decisions.md` design decisions |
| `editor/` | Zed extension and the language server source `tools/zxls.zig` |

## Error codes

```sh
zxc errors               # all error codes, grouped by category
zxc explain E0304        # meaning and fixes for one code
```

The error text lives in `src/errors.c`; `zxc errors` / `zxc explain` read it
directly, so there is nothing to look up by hand. The full reference is in
`docs/errors.md` (the `details:` links in diagnostics and clicking a code in
the editor both jump there).

## Docs

| File | Contents |
| --- | --- |
| `docs/manual.md` | getting started: first program through concurrency, JSON, and memory |
| `docs/reference.md` | quick reference: syntax, types, builtins, the full std API, tooling (version 1.4.2) |
| `docs/errors.md` | every error code explained (same as `zxc explain CODE`) |
| `spec/zx-spec.md` / `spec/decisions.md` | the language spec and design decisions |

The standard library is still evolving: the source order and comments in `std/`,
and editor completion (signatures + members), are always the most up-to-date
reference; there is no separate documentation site for now.
