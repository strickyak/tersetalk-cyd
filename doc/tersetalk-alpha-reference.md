# TerseTalk-Alpha Language & Library Reference

This document is a comprehensive reference for **TerseTalk-Alpha** (`../TerseTalk-Alpha`), a compact Smalltalk-inspired object-oriented language created in 2012–2013 by Henry Strickland & Thomas Shanks for on-device programming on mobile/touchscreen devices (Android + self-hosted web server).

It covers the language's lexical rules, syntax, operator precedence, execution model, built-in macros, image/world file format, VM classes and methods (`terse-vm`), standard prelude extensions (`prelude.txt`), and Android/Web UI & graphics libraries (`terse-a1`, `terse-web`).

---

## 1. Design Philosophy & Departures from Smalltalk

TerseTalk preserves Smalltalk's core uniform object/message-sending model and live-image ethos while redesigning syntax and collection APIs for minimal keystrokes on constrained keyboards and familiarity to Python programmers:

| Feature | Smalltalk-80 | TerseTalk-Alpha |
| :--- | :--- | :--- |
| **Case sensitivity** | Case-sensitive | **Case-insensitive** everywhere (classes, methods, variables, macros) |
| **Receiver / Super** | `self`, `super` | `me`, `up` |
| **Nil / Booleans** | `nil`, `true`, `false` | `nil` (`Undefined`), `1` and `0` (`Num`); Python-like truthiness |
| **Assignment** | `x := expr` | `x = expr` (plus tuple/list destructuring: `x, y = expr`) |
| **Method parameters** | Named in header (`at: idx put: val`) | **Implicitly named** `a`, `b`, `c`, `d`, ... in positional order |
| **Local variables** | Explicit `\| x y \|` declaration | **Implicitly declared** on assignment or block param (2-pass parser), like Python |
| **Return from method** | Explicit `^ expr` (with non-local block returns) | **No `^` operator and no non-local return**; value of last statement is returned |
| **Binary precedence** | All binary ops equal, left-to-right (`2 + 3 * 4` = `20`) | **4-level precedence**: `* / % &` > `+ - \| ^` > `@` > `== != < <= > >=` |
| **Chaining vs. Cascade** | `;` cascades to *original* receiver | `$` (or `,,`) chains to the **previous result** (setters return `me`) |
| **Vector literals** | `#(1 2 3)` or `{expr1. expr2}` | `()`, `a @ b @ c`, `a, b, c`, `a; b; c`, or `VEC(...)` |
| **Dict literals** | None in ANSI ST | `DICT(k1, v1; k2, v2)` or `DICT('k1' @ v1, 'k2' @ v2)` |
| **Blocks & Control flow** | `[ :x \| ... ]` + keyword sends (`ifTrue:ifFalse:`) | **`NAME(...)` Macros**: `FN(x: ...)`, `IF(...)THEN(...)ELSE(...)`, `FOR(...)MAP(...)` |
| **String interpolation** | None | `'x=[x] sum=[x + y]'` (escape literal brackets with `[[` and `]]`) |
| **Comments** | `"double quotes"` | `"double quotes"` in code; `#` at column 0 in world/prelude files |

---

## 2. Lexical Structure

### 2.1 Case-Insensitivity
All identifiers—class names, unary/keyword selectors, macro keywords, instance variables, and local variables—are converted to lowercase internally (`toLowerCase()`). `Foo`, `foo`, and `FOO` refer to the same class, variable, or message, though classes and local variables preserve original spelling for display.

### 2.2 Whitespace & Comments
- **Whitespace**: Any sequence of spaces, tabs, and newlines (`\s+`) separates tokens.
- **Code Comments**: Double-quoted strings `"..."` (`"[^\"]*"`) are treated as whitespace by the lexer (`TLex`). Comments cannot contain `"` or nest, and may span multiple lines.
- **World File Comments**: In `prelude.txt` and `w_*.txt` world files, lines starting with `#` in column 0 are ignored by `WorldReader`.

### 2.3 Identifiers & Numbers
- **Identifiers (`NAME`)**: `[A-Za-z][A-Za-z0-9_]*`.
- **Numbers (`NUMBER`)**: `-?[0-9]+(\.[0-9]+)?([Ee][+-]?[0-9]+)?`.
  - All numbers are 64-bit IEEE-754 floating-point values (`Ur.Num`).
  - `NaN` and `Infinity` are rejected at construction time, raising `NotANumber` or `IsInfinite`.
  - **Negative literal vs. subtraction disambiguation**: Expressions without spaces like `x-2` lex `-2` as `NUMBER`, which `Parser.parseBinary2` specially splits into binary `-` with argument `2`.

### 2.4 Strings & String Interpolation
- **String Literals (`STRING`)**: Enclosed in single quotes `'...'`.
  - **Escaping single quotes**: Double the single quote (`'What''s up'` $\rightarrow$ `What's up`). Backslash `\` is **not** an escape character in TerseTalk string literals.
  - Strings may span multiple lines.
- **String Interpolation**:
  - Inside `'...'`, `[expr]` parses `expr` as a block body and evaluates it (`value`) at runtime:
    ```tersetalk
    x, y = 23, 42.
    'x=[x] y=[y] sum=[x + y]'   "==> 'x=23 y=42 sum=65'"
    ```
  - **Escaping brackets**: `[[` produces a literal `[` and `]]` produces a literal `]`. Because regex character classes use `[...]`, regex strings in TerseTalk must double their brackets:
    ```tersetalk
    r = Rex new: 'w_([[a-z0-9]]+).txt'
    ```
  - Multi-part interpolated strings desugar to `(part1; part2; ...) jam`. *(Note: if a string consists solely of a single interpolation `'[expr]'` with no surrounding text, the parser emits `(expr) value` directly without `jam`.)*

### 2.5 Interchangeable Grouping Brackets
To make typing easier on mobile keyboards, the lexer treats **all** of the following as interchangeable open and close parentheses for grouping and macro blocks:
- **Open**: `(`, `{`, `[`, `!`
- **Close**: `)`, `}`, `]`, `?`

*(Note: `[` and `]` inside `'...'` perform string interpolation; outside strings they act like `(` and `)`. Idiomatic code in `prelude.txt` uses `( ... )` and `{ ... }` interchangeably.)*

### 2.6 Mobile Keyboard Substitutions (Bigraphs & Trigraphs)
Several multi-character punctuation aliases exist directly in `TLex` to avoid shifting keyboard layouts:
- `,,` $\equiv$ `$` (message chain)
- `.,` or `,.` $\equiv$ `;` (list separator)
- `!` and `?` $\equiv$ `(` and `)`

*(Parser also contains experimental `trigraphSubst` such as `/eq` $\rightarrow$ `==`, `/lt` $\rightarrow$ `<`, `/pl` $\rightarrow$ `+`, and `bigraphSubst` such as `..` $\rightarrow$ `:`, `--` $\rightarrow$ `=`, `(=` $\rightarrow$ `!=`, though whole-source graph substitution `noGraphSubst` is disabled in Alpha.)*

---

## 3. Syntax, Precedence, and Semantics

### 3.1 Complete Precedence & Associativity Table
From **highest** (tightest binding, 1) to **lowest** (loosest binding, 9):

| Level | Construct | Syntax / Operators | Associativity | Notes |
| :---: | :--- | :--- | :---: | :--- |
| **1** | **Primary** | `123`, `'str'`, `var`, `me`, `up`, `nil`, `(expr)`, `()`, `MACRO(...)` | — | `()` creates a new empty mutable `Vec` |
| **2** | **Unary Send** | `rcvr msg` | Left | `23 neg + 42` $\equiv$ `(23 neg) + 42` |
| **3** | **Binary Level 1** | `*`, `/`, `%`, `&` | Left | Multiplicative & bitwise AND |
| **4** | **Binary Level 2** | `+`, `-`, `\|`, `^` | Left | Additive & bitwise OR / XOR |
| **5** | **`@` Vector Builder** | `e1 @ e2 @ ...` | N-ary | Builds a `Vec`: `1+2 @ 3*4` $\equiv$ `VEC(3, 12)` |
| **6** | **Binary Level 3** | `==`, `!=`, `<`, `<=`, `>`, `>=` | Left | Relational & equality |
| **7** | **Keyword Send** | `rcvr kw1: a1 kw2: a2` | Single send | Args are Level 6; parenthesize nested keyword args |
| **8** | **Message Chain** | `expr $ msg1 $ msg2` (or `,,`) | Left | Sends subsequent msg(s) to **previous result** |
| **9** | **Comma Tuple** | `e1 , e2 , ...` | N-ary | Builds a `Vec` (trailing `,` optional: `(8,)`) |
| **10** | **Semicolon List** | `e1 ; e2 ; ...` (or `.,` / `,.`) | N-ary | Builds a `Vec` (trailing `;` optional: `(8;)`) |
| **11** | **Assignment** | `lvalue = stmt` | Right | Assigns local/inst var(s); evaluates to RHS |
| **12** | **Statement Seq** | `stmt1 . stmt2 . ...` | Left | Leading/trailing/repeated `.` allowed; returns last |

### 3.2 Message Sends & `$` Result Chaining
1. **Unary Messages**: Identifier not followed by `:` or `(`. E.g., `theta sin`, `v len`, `Foo new`.
2. **Binary Messages**: Single-argument operator messages at Precedence Levels 3, 4, and 6.
   - Example from `ParserTest`:
     `0 at: 1 != 2 + 3 * 4` parses as `0 at: (1 != (2 + (3 * 4)))`.
3. **Keyword Messages**: One or more `word:` tokens interleaved with arguments (`rcvr at: i put: x` sends selector `'at:put:'` with 2 arguments). Nested keyword sends in argument positions require parentheses: `d at: (v at: 0)`.
4. **Chaining (`$` or `,,`)**:
   - Unlike Smalltalk's `;` cascade (which re-sends to the *same* receiver), TerseTalk's `$` sends subsequent unary/binary/keyword messages to the **value returned by the left-hand expression**:
     ```tersetalk
     'one:more:time' split: ':' $ at: 2          "==> 'time'"
     'he' ap: 'l' $ ap: 'lo'                     "==> 'hello'"
     me triangle: (a - 1) $ + a                  "==> (me triangle: (a - 1)) + a"
     i / 19 $ cos * w + w                        "==> ((i / 19) cos) * w + w"
     ```
   - Because setter methods and `void` Java primitives return `me` (`self`), `$` also serves as a fluent builder cascade:
     ```tersetalk
     Cube new
       $ pos: 20 @ 30 @ 40
       $ scale: 5 @ 3 @ 1
       $ color: 1 @ 0 @ 0 @ 1
     ```

### 3.3 Three Ways to Build `Vec`s (`@`, `,`, `;`) + `VEC(...)`
TerseTalk provides three infix `Vec`-building operators at different precedence levels so nested vectors rarely need parentheses:
- **`@` (Level 5 — tightest, between `+` and `==`)**: Ideal for 2D/3D points, RGBA colors, and key-value pairs inside keyword arguments or comma lists:
  ```tersetalk
  me red line: 0 @ 0 to: wid - 1 @ hei - 1.
  DICT('type' @ 'live', 'value' @ blk)
  ```
- **`,` (Level 9 — looser than keywords and `$`)**: Tuple builder:
  ```tersetalk
  10, 20, 30        "==> VEC(10; 20; 30;)"
  (8,)              "==> 1-element Vec: VEC(8;)"
  ()                "==> 0-element empty mutable Vec: VEC()"
  ```
- **`;` (Level 10 — looser than `,`)**: List-of-tuples builder! Combining `,` and `;` creates a 2D list of pairs without inner parentheses:
  ```tersetalk
  111, 222; 333, 444;   "==> VEC(VEC(111; 222;); VEC(333; 444;);)"
  DICT('a', 1; 'b', 2)  "==> Dict with 'a'->1, 'b'->2"
  ```

### 3.4 Variables, Implicit Parameters, and Destructuring Assignment
- **Built-in Pseudo-Variables**:
  - `me`: Current receiver (`Frame.self`).
  - `up`: Superclass lookup proxy (`Terp.instSuper`). Evaluates with `me` as receiver, starting method lookup in `onCls.supercls`.
  - `nil`: Singleton `Undefined` value (`Terp.instNil`).
- **Implicit Method Parameters (`a`, `b`, `c`, ...)**:
  - Unary method (`foo`): 0 parameters.
  - Binary operator (`+`, `==`): 1 parameter named `a`.
  - Keyword method (`at:put:`): $k$ parameters (one per `:`) automatically named `a`, `b`, `c`, `d`, ...
- **Variable Scoping & Two-Pass Local Discovery**:
  - `Parser.parseMethod` parses every method **twice**:
    1. **Instance Variables**: Pre-populated from `onCls` and all its superclasses. Instance variables cannot shadow superclass instance variables.
    2. **Local Variables**: Pre-populated with `a`, `b`, ... for parameters. Pass 1 records every assigned variable (`x = ...`) and block parameter (`x:`) that is not an instance variable into the method's `localVars` table. At runtime, all locals start initialized to `nil`.
    3. **Global Variables**: Any identifier read in Pass 2 that is neither `me`/`up`/`nil`, an instance variable, nor a local variable resolves as a global lookup in `terp.clss` (the dictionary of all classes and metaclasses, e.g. `Vec`, `Dict`, `Num`, `Sys`, `File`). Globals cannot be assigned with `=`.
- **Destructuring Assignment**:
  - LValues can be single variable names (`x`), comma-separated tuples (`x, y, z`), or semicolon-separated lists (`x; y`):
    ```tersetalk
    x = y = 0.
    x, y = 3, 4.
    d1, d2, d3, d4 = r, d1, d2, d3.   "parallel swap/shift"
    ```
  - Destructuring requires the RHS to evaluate to a `Vec` of length $\ge$ number of LValue targets (extra elements in the RHS `Vec` are ignored).

### 3.5 Truthiness & Booleans
There is no separate `Boolean` class:
- `1` (`Terp.instTrue`) represents true; `0` (`Terp.instFalse`) represents false.
- **Falsy values** (`truth() == false`):
  1. `nil` (`Undefined`)
  2. `Num` values that round to integer `0` (`floor(x + 0.5) == 0`, i.e., $-0.5 \le x < 0.5$)
  3. Empty `Str` (`''`)
  4. Empty `Vec` (`()`)
  5. Empty `Dict` (`DICT()`)
  6. Empty `Bytes`
- **Truthy values** (`truth() == true`): All other objects (`Num` $\ne 0$, non-empty `Str`/`Vec`/`Dict`/`Bytes`, and all `Usr`, `Cls`, `Blk`, `Ht`, etc.).

### 3.6 Blocks, Macros, and Implicit Returns
- **No Non-Local Return**: Methods, statement sequences, and blocks always return the value of their last evaluated expression. Empty blocks `FN()` evaluate to `nil`.
- **Macro Syntax**:
  - Any identifier `NAME` immediately followed by `(`, `{`, `[`, or `!` (with optional whitespace, and **no** `:`) is parsed as a **Macro**, along with any immediately following `NAME2(...)` clauses:
    ```tersetalk
    IF(x > 0) THEN(y = 1) ELSE(y = -1)
    ```
  - Desugaring: `NAME1(b1) NAME2(b2) ...` compiles into a message send on `me`:
    ```tersetalk
    me macro: <currentFrame> name1: <Blk1> name2: <Blk2> ...
    ```
- **Block Parameters & Lexical Frame Sharing**:
  - Inside macro parentheses `(...)`, the parser expects zero or more `lvalue:` parameter declarations followed by a statement sequence:
    ```tersetalk
    FN(23 + 42)                  "0-arg block"
    FN(x: x * x)                 "1-arg block"
    FN(kind: xy: me on: kind at: xy)  "2-arg block"
    FOR(x, y: points) DO(...)    "1-arg block with destructuring tuple param!"
    ```
  - **Shared Frame**: A `Blk` captures the `Frame` of the enclosing method rather than allocating a separate activation frame. Assigning to locals or block parameters inside `FN(...)`, `IF(...)`, or `FOR(...)` reads and mutates the enclosing method's local/instance variables directly.
- **User-Defined Macros**:
  - Any class (or `Obj` globally) can define new macros simply by defining a `macro:...` method! Note that parameter `a` receives the caller's `Frame`, `b` receives the first `Blk`, `c` receives the second `Blk`, etc.:
    ```tersetalk
    meth Obj macro:MAYBE:NULL:
      x = b value.
      IF(x is: nil) THEN(c value) ELSE(x)

    "Usage:"
    MAYBE(query at: 'site') NULL('http://localhost/')
    ```

---

## 4. Built-in & Prelude Macros Reference

All built-in macros are defined as `macro:...` methods on `Obj` (so they are available everywhere unless the receiver is `Ur`/`Super`):

| Macro Syntax | Underlying Selector | Description |
| :--- | :--- | :--- |
| `FN( [p1: p2: ...] body )` | `macro:fn:` | Returns the compiled `Blk` closure object without evaluating it. |
| `IF(c) THEN(t)` | `macro:if:then:` | If `c` is truthy, evaluates and returns `t`; otherwise returns `nil`. |
| `IF(c) THEN(t) ELSE(f)` | `macro:if:then:else:` | If `c` is truthy, returns `t`; else returns `f`. |
| `IF(c1) THEN(t1) ELIF(c2) THEN(t2)` | `macro:if:then:elif:then:` | Chained conditional; returns `nil` if both `c1` and `c2` are falsy. |
| `IF(c1) THEN(t1) ELIF(c2) THEN(t2) ELSE(f)` | `macro:if:then:elif:then:else:` | Chained conditional with `ELSE` fallback. |
| `COND( p1, e1; p2, e2; ... )` | `macro:cond:` | Evaluates `p_i` in order; returns `e_i` for the first truthy `p_i`, or `nil` if none match. Pairs may use `,` or `@`. |
| `CASE(target) OF( k1, e1; k2, e2; ... )` | `macro:case:of:` | Evaluates `target` once, then tests `k_i equals: target` in order; returns matching `e_i` or `nil`. |
| `CASE(target) OF( ... ) ELSE(dflt)` | `macro:case:of:else:` | Like `CASE..OF`, evaluating `dflt` if no key matches. |
| `MAYBE(expr) NULL(dflt)` | `macro:maybe:null:` | *(Prelude)* Evaluates `expr`; if `expr is: nil`, evaluates and returns `dflt`, else returns `expr`. |
| `AND( e1; e2; ... )` | `macro:and:` | Short-circuiting logical AND over vector elements (`,` or `;`). Returns `1` if all are truthy, else `0`. |
| `OR( e1; e2; ... )` | `macro:or:` | Short-circuiting logical OR over vector elements (`,` or `;`). Returns `1` if any is truthy, else `0`. |
| `WHILE(cond) DO(body)` | `macro:while:do:` | Repeatedly evaluates `body` while `cond` evaluates truthy. Returns `me`. |
| `FOR( [k:] [v:] coll ) DO(body)` | `macro:for:do:` | Iterates over `coll` (`Num`, `Vec`, or `Dict` in sorted-key order); executes `body`; returns `nil`. See parameter binding rules below. |
| `FOR( ... ) MAP(expr)` | `macro:for:map:` | Like `FOR..DO`, returning a new `Vec` of `expr` results for each element. |
| `FOR( ... ) IF(cond) MAP(expr)` | `macro:for:if:map:` | List comprehension: evaluates `expr` only when `cond` is truthy, collecting results into a new `Vec`. |
| `FOR( ... ) INIT(acc: init) REDUCE(step)` | `macro:for:init:reduce:` | Fold/reduce: initializes `acc = init`, then for each item in `FOR` sets `acc = step`, returning final `acc`. |
| `TRY(body) CATCH(e: handler)` | `macro:try:catch:` | Evaluates `body`; if a `RuntimeException` / `TerseException` is thrown, binds `e` to `ex.toString()` (`Str`) and evaluates `handler`. |
| `VEC( ... )` | `macro:vec:` | Evaluates elements into a new `Vec`. `VEC()` $\rightarrow$ empty `Vec`; `VEC(x)` $\rightarrow$ 1-element `Vec`; `VEC(1, 2, 3)` $\rightarrow$ 3-element `Vec`. |
| `DICT( k1, v1; k2, v2; ... )` | `macro:dict:` | Constructs a `Dict` from 2-element `Vec` pairs (`DICT()` $\rightarrow$ empty `Dict`). |
| `HT( ... )` | `macro:ht:` | Concatenates `Ht` fragments (raw HTML), `Vec`s (recursively), and other objects (HTML-escaped `str`) into an XSS-safe `Ht`. |
| `TAG( 'name'; 'k', v; ... body ... )` | `macro:tag:` | Builds an HTML `<name k="v">body</name>` `Ht` object. First item is tag `Str`; subsequent 2-element `Vec`s become attributes; `Ht`s and scalars become body content. |

### `FOR(...)` Parameter Binding Rules (`Blk.storeAtParamKV`)
In all `FOR(...)` macros, `coll` may be a `Num` $n$ (iterates $0 \dots \text{round}(n)-1$), a `Vec` (iterates $0 \dots \text{len}-1$), or a `Dict` (iterates `sortedAssocs()`):
- **0 params** — `FOR(5) DO(...)`: Runs body $n$ times without binding variables.
- **1 param** — `FOR(v: coll)`:
  - `Num`: `v` receives $0, 1, \dots, n-1$.
  - `Vec`: `v` receives `coll at: i` (may destructure, e.g. `FOR(x, y: pairs)`).
  - `Dict`: `v` receives the **value** of each sorted `(key, value)` entry.
- **2 params** — `FOR(k: v: coll)`:
  - `Num`: `k` and `v` both receive $i$.
  - `Vec`: `k` receives index $i$, `v` receives `coll at: i`.
  - `Dict`: `k` receives `key`, `v` receives `value` (`FOR(k: v: dict)`).

---

## 5. Object Model, Metaclasses, and World Files

### 5.1 Class & Metaclass Hierarchy
TerseTalk uses a Smalltalk-80 parallel metaclass architecture:
- **`Ur`** is the root of the inheritance hierarchy (`Ur superCls` is `nil`).
- **`Obj`** is the immediate subclass of `Ur` for almost all objects (`Super` is the sole other direct subclass of `Ur`).
- Every class `X` is an object whose class (`X cls`) is `XCls`.
- Parallel inheritance: if `Bar` subclasses `Foo`, then `BarCls` subclasses `FooCls`.
- At the top of the class side: `ObjCls` $\rightarrow$ `UrCls` $\rightarrow$ `Cls` $\rightarrow$ `Obj` $\rightarrow$ `Ur`.
- Every `*Cls` is an instance of **`Metacls`** (`Metacls` $\rightarrow$ `Cls` $\rightarrow$ `Obj` $\rightarrow$ `Ur`, and `Metacls cls == MetaclsCls`).
- **Class methods** are simply instance methods defined on `XCls`.
- **Class variables** (class-instance variables) are simply instance variables defined on `XCls` (e.g., `XCls defVars: 'cache'`).
- **User Classes (`Usr` / `UsrCls`)**:
  - Built-in VM classes (`Num`, `Str`, `Vec`, `Dict`, etc.) are backed by specialized Java subclasses of `Obj`.
  - User-defined classes inherit from **`Usr`** (or its subclasses like `App`). `Usr`'s class `UsrCls` is an instance of Java's `Usr.UsrCls`, which provides generic instantiation (`new`), a 32-entry per-class LRU instance cache, numeric transient object IDs (`oid`), and named persistent instances (`find:`).
  - **`Tmp`** (subclass of `Usr`) is used to create throwaway contexts for top-level `eval:` (`Tmp new eval: '...'`).

### 5.2 World / Prelude Image Format (`prelude.txt`, `w_<world>.txt`)
TerseTalk persists live edits (new classes, instance variables, methods) by appending records to a world file `w_<world>.txt` (where `<world>` matches `^[a-z]{3}[0-9]{0,3}$`, e.g., `tmp0`, `web`). On startup, `Terp` loads `prelude.txt` followed by `w_<world>.txt`.

 Directives start at column 0 with a lowercase word; body lines are indented by at least 1 space (the first leading space is stripped):

```text
# Comment line (ignored)

cls SubName SuperName

vars ClassName
  x y z

meth ClassName selector:
  x = a.
  me

inst ClassName instanceName
  DICT('x', 10; 'y', 20; 'z', 30;)

equals 25
  x, y = 3, 4.  x*x + y*y
```

- **`cls <Sub> <Super>`** (alias `class`): Defines `<Sub>` as a subclass of `<Super>`.
- **`vars <Cls>`** (alias `instvars`): Defines whitespace-separated instance variables on `<Cls>` (or class variables if `<Cls>` ends in `Cls`). **Important**: When loaded via `InitialWorldReader` (`prelude.txt` / `w_*.txt`), `vars` **automatically generates** a getter `<v>` (`<v>`) and a fluent setter `<v>:` (`<v>=a. me`) for every variable! *(Calling `Cls >> defVars:` at runtime does not auto-generate getters/setters.)*
- **`meth <Cls> <selector>`**: Installs an uncompiled `UsrMeth` on `<Cls>` (lazily compiled on first invocation). If `<Cls>` does not exist yet, it is auto-created under `Orphan`.
- **`inst <Cls> <name>`**: Registers a serialized `Usr` instance (`<name>_<world>`) whose indented body evaluates to a `Dict` of field values when looked up via `<Cls> find: '<name>'`.
- **`equals <expectedExpr>`**: Boot-time self-test; evaluates `<expectedExpr>` and the indented body on `Tmp new` and throws an error unless `p1.equals(p2)`.
- **`stop`**: Halts loading the rest of the file.
- **Timestamped Wrapper**: Live edits appended at runtime wrap directives in `( <epochSecs> <directive...>` ... `) <epochSecs>`.

---

## 6. Core VM & Prelude Library Reference

*(Notation: `ClassName >> selector` denotes an instance method; `ClassName class >> selector` denotes a class method on `ClassNameCls`. Aliases/abbreviations generated by `wrap.tcl` or `JavaMeth` are shown after `/`.)*

### 6.1 Root & Object Protocol (`Ur`, `Super`, `Obj`, `Undefined`)

#### `Ur` (Superclass: *none*)
Root of all objects.
- `cls` $\rightarrow$ `Cls`: Returns the receiver's class object.
- `str` $\rightarrow$ `Str`: Human-readable string conversion (`toString()`; default `'ClassName~hash'`).
- `repr` $\rightarrow$ `Str`: Source-code-like representation (default `str`; `Str` quotes/escapes `'...'`; `Bytes` formats `(Hex en: '...')`).
- `truth` $\rightarrow$ `Num`: `1` if truthy, `0` if falsy.
- `hash` $\rightarrow$ `Num`: Value hash code (for `Num`, `Str`, `Vec`, `Dict`, `Bytes`) or identity hash.
- `equals:` `a` $\rightarrow$ `Num`: Structural/value equality (`1` or `0`), requiring matching classes.
- `does:` `selectorStr` $\rightarrow$ `Num`: `1` if receiver's class hierarchy implements `selectorStr`, else `0`.
- `peekVars` $\rightarrow$ `Dict`: Returns a `Dict` mapping all instance variable names (`Str`) to their current values.
- `pokeVars:` `dict` $\rightarrow$ `me`: Sets instance variables from `dict` keys/values.
- `dumpVarMap` $\rightarrow$ `me`: Logs instance variable layout and values to system log.

#### `Super` (Superclass: `Ur`)
Singleton class of `up` (`instSuper`). Has no `Obj` methods so almost any message sent to `up` triggers superclass method dispatch in `Expr.Send`.

#### `Obj` (Superclass: `Ur`)
Base class of all standard objects. Implements all built-in `macro:...` methods (Section 4) plus:
- **Identity & Types**:
  - `is:` `x` $\rightarrow$ `Num`: Object pointer identity (`1` if `me == x`, else `0`).
  - `null` $\rightarrow$ `Num`: *(Prelude)* `me is: nil`.
  - `isa:` `cls` $\rightarrow$ `Num`: `1` if `me` is an instance of `cls` or any subclass of `cls`, else `0`.
  - `sysHash` $\rightarrow$ `Num`: `System.identityHashCode(me)`.
- **Logic & Comparison**:
  - `not` $\rightarrow$ `Num`: Logical negation of `me truth` (`1` or `0`).
  - `==` `a`, `!=` `a`, `<` `a`, `<=` `a`, `>` `a`, `>=` `a`: *(Prelude)* Delegate to `me eq: a`, `me ne: a`, `me lt: a`, `me le: a`, `me gt: a`, `me ge: a` respectively.
- **Assertions, Errors & Logging**:
  - `must` $\rightarrow$ `me`: Throws exception if `me` is falsy.
  - `must:` `msgOrBlk` $\rightarrow$ `me`: Throws exception if `me` is falsy (evaluates `msgOrBlk` if it is a `Blk`).
  - `cant` / `cant:` `msgOrBlk` $\rightarrow$ `me`: Throws exception if `me` is truthy.
  - `err` / `err:` `detail` $\rightarrow$ *(throws)*: Raises a `TerseException` with `me` (and `detail`).
  - `say` / `say:` `detail` $\rightarrow$ `me`: Logs `#SAY# <me>` (and `<detail>`) to stderr/Android log and ring buffer (`Sys said`), returning `me`.
- **Dynamic Evaluation & Reflection**:
  - `eval:` `codeStr` $\rightarrow$ `Ur`: Parses and evaluates `codeStr` with `me` as receiver.
  - `eval:arg:` `codeStr` `a` $\rightarrow$ `Ur`: Evaluates `codeStr` with `me` as receiver and parameter `a`.
  - `eval:arg:arg:` `codeStr` `a` `b` $\rightarrow$ `Ur`: Evaluates `codeStr` with `me` as receiver and parameters `a`, `b`.
  - `apply:args:` `selectorStr` `argsVec` $\rightarrow$ `Ur`: Dynamically invokes a `UsrMeth` named `selectorStr` on `me` with `argsVec`.
- **Misc**:
  - `nearestInt` $\rightarrow$ `Num`: Rounds a `Num` to nearest integer via `floor(x + 0.5)` (errors if not `Num`).
  - `flattenTo:` `destVec` $\rightarrow$ `me`: *(Prelude)* Appends `me` to `destVec` (overridden by `Vec` for recursive flattening).

#### `Undefined` (Superclass: `Obj`)
Class of singleton `nil`.
- `truth` $\rightarrow$ `0`.
- `str` $\rightarrow$ `'Nil'`.

---

### 6.2 Numbers (`Num`)

#### `Num` (Superclass: `Obj`)
Immutable 64-bit float. Falsy iff `round == 0`. Prints without decimal point when integer-valued.
- **Class Methods (`NumCls`)**:
  - `Num rand` $\rightarrow$ `Num`: Uniform random float in $[0.0, 1.0)$.
  - `Num rand:` `n` $\rightarrow$ `Num`: Uniform random integer in $\{0, 1, \dots, n-1\}$.
  - `Num pi` $\rightarrow$ `Num`: $\pi \approx 3.141592653589793$.
  - `Num tau` $\rightarrow$ `Num`: $2\pi \approx 6.283185307179586$.
  - `Num e` $\rightarrow$ `Num`: $e \approx 2.718281828459045$.
- **Arithmetic & Bitwise Operators**:
  - `+` `a`, `-` `a`, `*` `a`, `/` `a` $\rightarrow$ `Num`: Floating-point arithmetic.
  - `%` `a` $\rightarrow$ `Num`: Floating-point modulo with **non-negative residue** (adds `a` if remainder $< 0$).
  - `idiv:` `a`, `imod:` `a` $\rightarrow$ `Num`: 64-bit truncated integer division `(long)me / (long)a` and remainder `(long)me % (long)a`.
  - `pow:` `a` $\rightarrow$ `Num`: `Math.pow(me, a)`.
  - `&` `a`, `|` `a`, `^` `a` $\rightarrow$ `Num`: 32-bit signed integer bitwise AND, OR, XOR.
- **Comparisons**:
  - `==` / `eq:`, `!=` / `ne:`, `<` / `lt:`, `<=` / `le:`, `>` / `gt:`, `>=` / `ge:` $\rightarrow$ `Num` (`1` or `0`).
- **Unary Math & Conversions**:
  - `neg` (`-x`), `abs` (`|x|`), `sgn` (`-1`, `0`, `1`), `sq` ($x^2$), `sqrt` ($\sqrt{x}$).
  - `int` (truncate toward 0 via `(int)`), `floor` ($\lfloor x \rfloor$), `round` ($\lfloor x + 0.5 \rfloor$), `num` (returns `me`).
  - `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `sinh`, `cosh`, `tanh`.
  - `ln`, `log10`, `exp` *(Note: buggy in Alpha—see Section 8)*.
  - `range` $\rightarrow$ `Vec`: Returns `VEC(0, 1, ..., round(me) - 1)`.
  - `chr` $\rightarrow$ `Str`: 1-character Unicode `Str` for codepoint `round(me)`.
  - `fmt:` `printfFmtStr` $\rightarrow$ `Str`: Formats float using Java `String.format` (e.g. `fps fmt: '%.2f'`).

---

### 6.3 Strings, String Buffers, Byte Arrays & Regex (`Str`, `Buf`, `Bytes`, `Rex`)

#### `Str` (Superclass: `Obj`)
Immutable UTF-16 string. Falsy iff empty (`''`).
- **Length, Indexing & Slicing**:
  - `length` / `len` $\rightarrow$ `Num`: Number of characters.
  - `head` / `hd` $\rightarrow$ `Str`: First character as a 1-char `Str` (errors if empty).
  - `tail` / `tl` $\rightarrow$ `Str`: All characters after the first (errors if empty).
  - `head:` `n` $\rightarrow$ `Str`: *(Prelude)* First `n` characters (or `me` if `len < n`).
  - `cut:to:` / `sub:to:` `start` `end` $\rightarrow$ `Str`: Substring `[start, end)` (Java `substring`).
- **Concatenation, Splitting & Trimming**:
  - `append:` / `ap:` `str` $\rightarrow$ `Str`: Returns new concatenated `Str` (`me + str`; argument must be a `Str`).
  - `split:` `delimStr` $\rightarrow$ `Vec`: Splits on `delimStr`'s first character, **omitting empty segments** (`splitNonEmpty`).
  - `trimWhite` / `trimw` $\rightarrow$ `Str`: Strips leading and trailing whitespace.
  - `lower` / `low`, `upper` / `upp` $\rightarrow$ `Str`: Case conversion.
- **Comparisons & Matching** (note: `Obj` defines `==`, `<`, etc. to call `eq:`, `lt:`, etc.):
  - `eq:`, `ne:`, `lt:`, `le:`, `gt:`, `ge:` `otherStr` $\rightarrow$ `Num`: Lexicographical comparison (`1` or `0`).
  - `starts:` `prefix`, `ends:` `suffix` $\rightarrow$ `Num`: Prefix/suffix test (`1` or `0`).
  - `matchp:` `regexStr` $\rightarrow$ `Num`: Full-string Java regex match (`Pattern.matches`).
- **Conversions**:
  - `toNumber` / `num` $\rightarrow$ `Num`: Parses float string (errors if invalid).
  - `ord` $\rightarrow$ `Num`: Unicode codepoint of first character (`charAt(0)`).
  - `explode` $\rightarrow$ `Vec`: `Vec` of character codepoint `Num`s.
  - `bytes` $\rightarrow$ `Bytes`: UTF-8 encoded `Bytes`.
  - `utf8` / `utf` $\rightarrow$ `Bytes`: *(Prelude)* `Utf8 en: me`.
  - `curly` $\rightarrow$ `Bytes`: *(Prelude)* `Curly en: me`.
  - `applySubstitutions` $\rightarrow$ `Str`: Hook for keyboard graph substitutions (no-op in Alpha).

#### `Buf` (Superclass: `Obj`)
Mutable string builder (`StringBuffer`). Convert to `Str` with `str`.
- **Class Methods (`BufCls`)**:
  - `Buf new` $\rightarrow$ `Buf`: Empty buffer.
  - `Buf append:` / `Buf ap:` `obj` $\rightarrow$ `Buf`: New buffer initialized with `obj str`.
- **Instance Methods**:
  - `append:` / `ap:` `obj` $\rightarrow$ `me`: Appends `obj str` in place and returns `me`.

#### `Bytes` (Superclass: `Obj`)
Immutable byte array (`byte[]`). Falsy iff length 0. `str` decodes as UTF-8.
- **Class Methods (`BytesCls`)**:
  - `Bytes implode:` `vecOfNums` $\rightarrow$ `Bytes`: Packs a `Vec` of integer `Num`s (low 8 bits) into `Bytes`.
- **Instance Methods**:
  - `head:` `n` $\rightarrow$ `Bytes`: First `n` bytes `[0, n)`.
  - `tail:` `n` $\rightarrow$ `Bytes`: Last `n` bytes `[len - n, len)`.
  - `cut:to:` `p` `q` $\rightarrow$ `Bytes`: Byte slice `[p, q)`.
  - `cut:len:` `p` `len` $\rightarrow$ `Bytes`: Byte slice `[p, p + len)`.
  - `explode` $\rightarrow$ `Vec`: `Vec` of signed byte `Num`s (`-128`..`127`).
  - *(Prelude)* `sha1` (`Sha1 en: me`), `hex` (`Hex en: me`), `deHex` (`Hex de: me`), `deCurly` (`Curly de: me`), `deUtf8` / `deUtf` (`Utf8 de: me`).

#### `Rex` (Superclass: `Obj`)
Compiled regular expression (`java.util.regex.Pattern`).
- **Class Methods (`RexCls`)**:
  - `Rex new:` `patternStr` $\rightarrow$ `Rex`: Compiles regex pattern. (Remember to double `[` and `]` inside single-quoted string literals!)
- **Instance Methods**:
  - `match:` `str` $\rightarrow$ `Vec` or `nil`: Anchored prefix match (`Matcher.lookingAt()`). Returns `nil` if no match, or a `Vec` of `Str`s `[fullMatch, group1, group2, ...]`.

---

### 6.4 Collections (`Vec`, `Dict`)

#### `Vec` (Superclass: `Obj`)
Mutable dynamic array (`ArrayList<Ur>`). Falsy iff empty. Value-comparable (`equals:`, `hash`).
- **Class Methods (`VecCls`)**:
  - `Vec new` $\rightarrow$ `Vec`: Creates an empty `Vec` (equivalent to `()` or `VEC()`).
  - `Vec append:` / `Vec ap:` `elem` $\rightarrow$ `Vec`: Creates a 1-element `Vec` containing `elem`.
  - `Vec pairs:` `vecOfKeyIndexPairs` $\rightarrow$ `Vec`: *(Prelude)* Populates a `Vec` via `z at: k put: v` for each `k, v` pair.
- **Indexing, Mutation & Slicing**:
  - `len` $\rightarrow$ `Num`: Number of elements.
  - `at:` `i` $\rightarrow$ `Ur`: Returns element at `round(i)` **modulo `len`** (`((i % len) + len) % len`). Supports Python-style negative indices (`v at: -1` is last element) AND toroidal wrap-around for `i >= len`! Errors on empty `Vec`.
  - `at:put:` / `at:p:` `i` `val` $\rightarrow$ `me`: Replaces element at `round(i)` **modulo `len`** with `val`; returns `me`.
  - `append:` / `ap:` `val` $\rightarrow$ `me`: Appends `val` to the end in place; returns `me`.
  - `cat:` `otherVec` $\rightarrow$ `Vec`: Returns a new `Vec` concatenating `me` and `otherVec`.
  - `cut:to:` `begin` `end` $\rightarrow$ `Vec`: Returns subvector `[begin, end)` (negative indices have `len` added once; clamped at `len`).
  - `cut:len:` `begin` `length` $\rightarrow$ `Vec`: Returns subvector `[begin, begin + length)`.
  - `x`, `y`, `z`, `w` $\rightarrow$ `Ur`: *(Prelude)* Shorthand getters for `me at: 0`, `me at: 1`, `me at: 2`, `me at: 3`.
  - `flat` $\rightarrow$ `Vec`: *(Prelude)* Recursively flattens nested `Vec`s into a new 1D `Vec`.
  - `flattenTo:` `destVec` $\rightarrow$ `me`: *(Prelude)* Appends flattened elements into `destVec`.
- **String Joining & Imploding**:
  - `join:` `sepStr` $\rightarrow$ `Str`: Joins element `str`s separated by `sepStr`.
  - `join` $\rightarrow$ `Str`: Joins element `str`s separated by single spaces `' '`.
  - `jam` $\rightarrow$ `Str`: Joins element `str`s with **no** separator (`''`).
  - `implode` / `imp` $\rightarrow$ `Str`: Recursively concatenates elements where `Num`s are converted to Unicode characters (`chr`), nested `Vec`s are recursed, and other objects use `str`.
- **Vector Math & Comparisons**:
  - `abs` $\rightarrow$ `Num`: Euclidean norm $\sqrt{\sum x_i^2}$.
  - `unit` $\rightarrow$ `Vec`: Normalized unit vector `me / me abs` (errors if `abs == 0`).
  - `dot:` `otherVec` $\rightarrow$ `Vec`: Elementwise product `[me[i] * otherVec[i]]` up to `min(len1, len2)`. *(Note: returns a `Vec`, not a scalar sum!)*
  - `cross:` `otherVec` $\rightarrow$ `Vec`: 3D cross product (requires both `Vec`s to have length 3).
  - `eq:` `otherVec` $\rightarrow$ `Vec`: *(Prelude, also invoked by `==`)* Elementwise `==` returning a `Vec` of `1`s and `0`s (raises error if lengths differ or arg is not `Vec`; use `equals:` for scalar boolean equality!).

#### `Dict` (Superclass: `Obj`)
Mutable hash table (`HashMap<Ur, Ur>`). Falsy iff empty. Value-comparable (`equals:`, `hash`). Any `Ur` with value equality (`Str`, `Num`, `Vec`, etc.) can be a key.
- **Class Methods (`DictCls`)**:
  - `Dict new` $\rightarrow$ `Dict`: Creates an empty `Dict` (equivalent to `DICT()`).
- **Instance Methods**:
  - `len` $\rightarrow$ `Num`: Number of key-value entries.
  - `dir` $\rightarrow$ `Vec`: Returns a **sorted** `Vec` of all keys in the `Dict`.
  - `at:` `key` $\rightarrow$ `Ur`: Returns value associated with `key`, or **`nil`** if absent (never throws KeyError).
  - `at:put:` `key` `val` $\rightarrow$ `me`: Associates `key` with `val` in place and returns `me`.

---

### 6.5 Blocks (`Blk`)

#### `Blk` (Superclass: `Obj`)
Lexical block closure created via `FN(...)` or passed as macro arguments.
- `value` $\rightarrow$ `Ur`: Evaluates block body with 0 arguments in its captured `Frame`.
- `value:` `a` $\rightarrow$ `Ur`: Stores `a` into param 0 (requires $\ge 1$ param) and evaluates body.
- `value:value:` `a` `b` $\rightarrow$ `Ur`: Stores `a`, `b` into params 0, 1 (requires $\ge 2$ params) and evaluates body.
- `storeAtParam0:` `x`, `storeAtParam1:` `x`, `storeAtParam:value:` `k` `v` $\rightarrow$ `me`: Low-level parameter-binding helpers used by loop macros.

---

### 6.6 Classes, Methods, User Objects & Reflection (`Cls`, `Meth`, `Usr`, `Tmp`)

#### `Cls` (Superclass: `Obj`)
Represents classes and metaclasses.
- **Class Methods (`ClsCls`)**:
  - `Cls all` $\rightarrow$ `Dict`: `Dict` mapping lowercase class names (`Str`) to `Cls` objects (includes `*Cls` metaclasses).
  - `Cls at:` `nameStr` $\rightarrow$ `Cls` or `nil`: Case-insensitive lookup of a class by name.
- **Instance Methods**:
  - `name` $\rightarrow$ `Str`: Class name preserving original capitalization.
  - `superCls` / `sup` $\rightarrow$ `Cls` or `nil`: Superclass (`nil` for `Ur`).
  - `subs` $\rightarrow$ `Vec`: Sorted `Vec` of direct subclass names (`Str`, lowercase).
  - `defSub:` `newNameStr` $\rightarrow$ `Cls`: Creates a new subclass `newNameStr` (and metaclass `newNameStrCls`), registers both in globals, and appends `cls` to the world file.
  - `vars` $\rightarrow$ `Vec`: `Vec` of instance variable names (`Str`) declared directly on this class.
  - `defVars:` / `vars:` `spaceSepNamesStr` $\rightarrow$ `me`: Sets this class's instance variables (checking case-insensitive uniqueness across superclasses) and appends `instvars` to the world file.
  - `meths` / `allMeths` $\rightarrow$ `Dict`: `Dict` of methods defined directly on this class (`selectorStr` $\rightarrow$ `Meth`).
  - `at:` `selectorStr` $\rightarrow$ `Meth` or `nil`: Looks up a method defined directly on this class (`nil` if absent).
  - `def:c:` `selectorStr` `codeStr` $\rightarrow$ `me`: Compiles and installs a `UsrMeth` on this class and appends `meth` to the world file.
  - `defineMethod:abbrev:doc:code:` / `defMeth:a:d:c:` `sel` `abbrev` `doc` `code` $\rightarrow$ `me`: Full method definition with optional alias selector `abbrev` and docstring `doc`.
  - `trace:` `bool` $\rightarrow$ `me`: Enables/disables debug trace logging for message sends and instvar writes on this class.

#### `Meth`, `JavMeth`, `UsrMeth` (Superclass: `Obj`)
Method objects stored in `Cls >> meths`.
- **`Meth`**: `onCls` ($\rightarrow$ `Cls`), `name` ($\rightarrow$ `Str`), `abbrev` ($\rightarrow$ `Str`), `doc` ($\rightarrow$ `Str`), `applyFrame:receiver:args:` `frame` `rcvr` `argsVec` ($\rightarrow$ `Ur`).
- **`UsrMeth`** (subclass of `Meth`): `src` ($\rightarrow$ `Str`, source code; also returned by `str`), `top` ($\rightarrow$ `MethTop`, lazily compiled AST root).

#### `Usr` & `Tmp` (Superclass: `Obj`)
Base class for user-defined objects (`Tmp` is an empty subclass of `Usr` for scratch evaluation).
- **Class Methods (`UsrCls` — inherited by all subclasses of `Usr`)**:
  - `new` $\rightarrow$ `Usr`: Allocates a new instance with all `instVars` set to `nil` and stores it in the class's 32-slot LRU cache.
  - `find:` `idOrNameStr` $\rightarrow$ `Usr` or `nil`: Finds an instance by numeric `oid` in the LRU cache, or realizes a saved `inst` by name (`name` or `name_world`).
- **Instance Methods**:
  - `oid` / `omention` $\rightarrow$ `Num`: Assigns (if 0) and returns a transient integer ID, refreshing the object in the class's LRU cache.
  - `oname` $\rightarrow$ `Str`: Persistent name if loaded from `inst`, otherwise `oid str`.
  - `opath` $\rightarrow$ `Str`: `'oname@ClassName'` (resolvable by `Sys find:`).
  - `nonMain` $\rightarrow$ `me`: *(Prelude)* Marker method used by `BrowseClasses` to distinguish helper classes from top-level launchable apps.

---

### 6.7 System, Files & Web Hub (`Sys`, `File`, `Hub`)

#### `Sys` (Superclass: `Obj`)
System utilities (all class methods on `SysCls`):
- `Sys secs` $\rightarrow$ `Num`: Current Unix epoch time in seconds (`double`, millisecond resolution).
- `Sys nanos` $\rightarrow$ `Num`: `System.nanoTime()`.
- `Sys sleep:` `seconds` $\rightarrow$ `Sys`: Sleeps current thread for `seconds` (float).
- `Sys said` $\rightarrow$ `Vec`: Returns recent log messages (up to 128 entries, newest first) as a `Vec` of `Str`s.
- `Sys find:` `'oname@ClassName'` $\rightarrow$ `Obj`: Resolves `'@ClassName'` to `Cls`, or `'id@UsrSubClass'` via `UsrCls >> find:`.
- `Sys worldName` $\rightarrow$ `Str`: Name of currently loaded world (e.g. `'tmp0'`, `'web'`).
- `Sys worldFileName` $\rightarrow$ `Str`: Filename of active world log (e.g. `'w_tmp0.txt'`).
- `Sys fail` / `Sys fail:` `msg` $\rightarrow$ *(throws)*: Throws a `TerseException`.
- `Sys trigraphs` $\rightarrow$ `Dict`: Returns the `/xx` keyboard substitution `Dict`.

#### `File` (Superclass: `Obj`)
Local sandboxed `.txt` file I/O (all class methods on `FileCls`; filenames must match `^[a-z][a-z0-9_]{1,31}\.txt$`):
- `File dir` $\rightarrow$ `Vec`: `Vec` of 3-tuples `VEC(filenameStr, mtimeSecsNum, byteLenNum)` for all `.txt` files.
- `File read:` `filename` $\rightarrow$ `Str`: Reads text file (normalizing line endings to `\n`).
- `File write:value:` `filename` `contentStr` $\rightarrow$ `File`: Overwrites text file.
- `File append:value:` `filename` `contentStr` $\rightarrow$ `File`: Appends to text file.
- `File delete:` `filename` $\rightarrow$ `File`: Deletes file.

#### `Hub` (Superclass: `Obj`)
Remote wiki/hub `.txt` file sharing (`http://wiki.yak.net/1017`, implemented in `AndyTerp`; throws unimplemented on `PosixTerp`):
- `Hub dir` $\rightarrow$ `Vec`: Remote `Vec` of `VEC(name, mtime, size)` tuples.
- `Hub read:` `filename` $\rightarrow$ `Str`: Downloads text file from Hub.
- `Hub write:value:` `filename` `contentStr` $\rightarrow$ `Hub`: Uploads text file to Hub.

---

### 6.8 XSS-Safe HTML Builder (`Ht`)

#### `Ht` (Superclass: `Obj`)
Wraps `Ur.Html` so raw HTML markup (`Ht`) and untrusted strings (`Str`, auto-escaped for `&`, `<`, `>`, `"`) compose safely (typically used alongside `HT(...)` and `TAG(...)` macros).
- **Class Methods (`HtCls`)**:
  - `Ht new:` `strOrHt` $\rightarrow$ `Ht`: Creates an `Ht` (escaping `Str`, preserving `Ht`).
  - `Ht entity:` `nameStr` $\rightarrow$ `Ht`: HTML entity `&name;` (e.g., `Ht entity: 'alpha'`).
  - `Ht tag:params:body:` `tagStr` `pairsVecOrNil` `bodyOrNil` $\rightarrow$ `Ht`: Builds `<tag k="v">body</tag>`.
  - *(Prelude helpers)*:
    - `Ht nbsp` $\rightarrow$ `Ht`: `&nbsp;`.
    - `Ht bold:` `x` $\rightarrow$ `Ht`: `<b>x</b>`.
    - `Ht box:` `x` $\rightarrow$ `Ht`: 1-cell bordered `<table>`.
    - `Ht link:to:` `label` `url` $\rightarrow$ `Ht`: `<a href="url">label</a>`.
    - `Ht linkLabelPairs:` `vec` $\rightarrow$ `Ht`: Formats `(url, label)` or `(url, label, extra)` tuples into spaced links.
    - `Ht vec:` `v` / `Ht ul:` `v` $\rightarrow$ `Ht`: Bulleted `<ul><li>...</li></ul>` list.
    - `Ht dict:` `d` / `Ht vecvec:` `pairs` / `Ht dl:` `pairs` $\rightarrow$ `Ht`: `<dl><dt>...</dt><dd>...</dd></dl>` definition list.
- **Instance Methods**:
  - `append:` / `ap:` `strOrHt` $\rightarrow$ `me`: Appends `Ht` (raw) or other object (HTML-escaped) in place.

---

### 6.9 Encodings, Serialization, Crypto & HTTP (`terse.vm.More`)

All codec classes (except `DhSecret`, `Aes`, `Client`) are static utility classes whose methods live on `*Cls`:

| Class | Methods | Description |
| :--- | :--- | :--- |
| **`Json`** | `Json en: obj` $\rightarrow$ `Str`<br>`Json de: strOrBytes` $\rightarrow$ `Obj` | Encodes/decodes `Num`, `Str`, `Vec`, `Dict`, and `nil` (`null`) to/from JSON (`Dict` keys sorted; supports non-string keys!). |
| **`Pickle`** | `Pickle en: usrObj` $\rightarrow$ `Str`<br>`Pickle de: strOrBytes` $\rightarrow$ `Usr` | Serializes/deserializes arbitrary cyclic graphs of `Usr` instances (plus `Num`, `Str`, `Vec`, `Dict`, `nil` fields) as JSON object tables keyed by `'serial@salt@ClassName'`. |
| **`QueryStr`** | `QueryStr en: dict` $\rightarrow$ `Str`<br>`QueryStr de: str` $\rightarrow$ `Dict` | URL query-string (`k1=v1&k2=v2`) encoding and decoding. |
| **`EscStr`** | `EscStr en: str` $\rightarrow$ `Str` | Escapes `'`, `[`, `]` as `''`, `[[`, `]]` for embedding inside TerseTalk string literals. |
| **`Utf8`** | `Utf8 en: str` $\rightarrow$ `Bytes`<br>`Utf8 de: bytes` $\rightarrow$ `Str` | UTF-8 encoding/decoding between `Str` and `Bytes`. |
| **`Hex`** | `Hex en: bytesOrStr` $\rightarrow$ `Bytes`<br>`Hex de: hexBytes` $\rightarrow$ `Bytes` | Lowercase hexadecimal encoding and decoding. |
| **`Curly`** | `Curly en: str` $\rightarrow$ `Bytes`<br>`Curly de: bytes` $\rightarrow$ `Str` | ASCII-safe encoding escaping any char outside `'#'..'`'` (and empty string `{}`) as `{decimalCodepoint}`. |
| **`Sha1`** | `Sha1 en: bytes` $\rightarrow$ `Bytes` | 20-byte SHA-1 digest. |
| **`DhSecret`** | `DhSecret rand` $\rightarrow$ `DhSecret`<br>`DhSecret new: hexStr` $\rightarrow$ `DhSecret`<br>`dh pub` $\rightarrow$ `Str`<br>`dh mutual: peerPubHexStr` $\rightarrow$ `Str` | 1536-bit Diffie-Hellman key exchange (RFC 3526 MODP Group 5, $g=2$). Keys and shared secrets are lowercase hex `Str`s. |
| **`Aes`** | `Aes key: bytes16` $\rightarrow$ `Aes`<br>`aes en: plainBytes` $\rightarrow$ `Bytes`<br>`aes de: cipherBytes` $\rightarrow$ `Bytes` | Authenticated AES-128-CBC encryption with PKCS5 padding, random 16-byte IV prefix, and prepended 16-byte truncated SHA-1 plaintext integrity check. |
| **`Client`** | `Client host: h port: p` $\rightarrow$ `Client`<br>`cli get: path query: qDict` $\rightarrow$ `Str`<br>`cli post: path query: qDict form: fDict` $\rightarrow$ `Str` | Simple HTTP/1.1 client (`path` must start with `/`). |

---

### 6.10 AST, Parser & Stack Frame Reflection (`Expr`, `Frame`)

TerseTalk exposes its AST and execution frames as first-class objects:
- **`Frame`**: `prevFrame` ($\rightarrow$ `Frame` or `nil`), `receiver` ($\rightarrow$ `Ur`), `localVars` ($\rightarrow$ `Vec`), `currentMethodExpr` ($\rightarrow$ `MethTop`), `level` ($\rightarrow$ `Num`).
- **`Expr`** (base AST node): `evalFrame:` `frame` ($\rightarrow$ `Ur`), `depth` ($\rightarrow$ `Num`), `white` / `front` / `rest` ($\rightarrow$ `Str` parser slices).
- **`MethTop`** (method root, returned by `UsrMeth >> top`): `onCls` ($\rightarrow$ `Cls`), `methName` ($\rightarrow$ `Str`), `numArgs` ($\rightarrow$ `Num`), `numLocals` ($\rightarrow$ `Num`), `body` ($\rightarrow$ `Expr`), `source` ($\rightarrow$ `Str`), `sends` ($\rightarrow$ `Dict` mapping every sent selector `Str` to a `Vec` of character offsets!).
- **`Send`**: `rcvr` ($\rightarrow$ `Expr`), `msg` ($\rightarrow$ `Str`), `args` ($\rightarrow$ `Vec` of `Expr`), `sourceLoc` ($\rightarrow$ `Vec` of `Num`).
- **`LValue`** (`LvName`, `LvLocalName`, `LvInstName`, `LvTuple`, `LvList`): `storeFrame:value:`, `recallFrame:`, `name`, `index`, `arr`.
- Other AST subclasses: `PutLValue`, `GetInstVar`, `GetLocalVar`, `GetSelf`, `GetFrame`, `GetGlobalVar`, `Block`, `Seq`, `MakeVec`, `Lit`, `EmptyExprList`, `Parser`.

---

## 7. Application Architecture, Graphics & Platform Libraries

### 7.1 URL Dispatch & Response `Dict` Protocol
Both the Android app (`TerseActivity`) and desktop web server (`WebServer`) route every screen/request as a URL `/AppName[.instanceId][.extra...]` with query parameters `Dict` `q`:
1. If `.instanceId` matches a cached `Usr` instance, sends `inst handle: path query: q`.
2. Else if `AppNameCls` understands `handle:query:`, sends `AppName handle: path query: q`.
3. Else instantiates `AppName new` (caching its `oid`) and sends `inst handle: path query: q`.

`handle:query:` returns a `Dict` whose `'type'` key selects the renderer:

| `'type'` | `'value'` Type | Extra Keys | Rendered View (Android / Web) |
| :--- | :--- | :--- | :--- |
| `'text'` | `Str` | `'title'` | Monospace text view (`<pre>` on Web) |
| `'list'` | `Vec` | `'title'` | Interactive `ListView` (`<table>` on Web); items starting with `/Path label` or `\|link\|/Path\|Label\|Text` are clickable links |
| `'edit'` | `Str` | `'title'`, `'action'`, `'field1'`/`'value1'`, `'field2'`/`'value2'` | Multi-line code/text editor with Save/Submit posting edited `'text'` to `'action'` URL |
| `'html'` | `Ht` | `'title'` | Embedded `WebView` intercepting links back into `TerseActivity` (direct HTML on Web) |
| `'draw'` | `Vec` | `'width'`, `'height'`, `'url'` | Retained 2D vector list (`VEC('line', x1, y1, x2, y2)`, `VEC('rect', x, y, w, h)`, `VEC('text', x, y, str)`); touch-up reloads `'url'` with `ex`, `ey` (SVG on Web) |
| `'live'` | `Blk` | `'event'` (`Blk`) | *(Android)* Fullscreen double-buffered `SurfaceView` running `value` block on a background `WorkThread` with a `Screen` arg, dispatching touch events to `event` (`FN(kind: xy: ...)`) |
| `'fnord'` / `'dual'` | `Obj` | — | *(Android)* OpenGL ES 1.0 3D `GLSurfaceView` + 2D `OverView` HUD; invokes `app run: ggl` on background `WorkThread` |
| `'usr'` | `BaseViewPrim` | — | *(Android)* Experimental native Android `View`/`LinearLayout` wrapper |
| `'world'` | `Str` | — | *(Android)* Switches active world to `'value'` and reloads `AndyTerp` |

### 7.2 Android 2D Live Graphics (`Screen`, `Ink`, `Motion`)
Used with `'type' -> 'live'` (e.g. `LiveApp`, `DrawAct`, `GamePong`, `GameTroids`, `DemoLiveLife`):
- **3-Digit RGB Color Numbers**: Colors are specified as integers `000`–`999` where the hundreds digit is Red (`0`–`9`), tens is Green (`0`–`9`), and ones is Blue (`0`–`9`), e.g. `900` = red, `090` (`90`) = green, `009` (`9`) = blue, `999` = white, `0` = black.
- **`Screen`**:
  - `width`, `height` $\rightarrow$ `Num`: Canvas dimensions in pixels.
  - `clear:` `rgb999` $\rightarrow$ `me`: Fills offscreen bitmap with color.
  - `newInk:` `rgb999` $\rightarrow$ `Ink`: Creates a brush/paint bound to this `Screen`.
  - `post` $\rightarrow$ `me`: Blits offscreen bitmap to the `SurfaceHolder`, updates FPS counters, and **drains queued touch `Motion` events** by invoking the app's `event` block `FN(action: xy: ...)`.
  - `fps` $\rightarrow$ `Num` or `nil`: Average frames per second since first `post` (`nil` until $>1$ post).
- **`Ink`**:
  - `scr` $\rightarrow$ `Screen`: Owning screen.
  - `color:` `rgb999`, `thick:` `pixels`, `fontsize:` `px` $\rightarrow$ `me`: Configures brush style.
  - `line:to:` `xy1` `xy2`, `rect:to:` `xy1` `xy2`, `circ:r:` `xy` `radius`, `dot:` `xy`, `dots:` `vecOfXY`, `text:sw:` `str` `xy` $\rightarrow$ `me`: Draws onto `scr`'s offscreen canvas (`sw:` is southwest text baseline origin).
- **`Motion`**: `x`, `y`, `action` (`0` = `ACTION_DOWN`, `2` = `ACTION_MOVE`).

### 7.3 Android 3D OpenGL ES & 2D HUD Scene Graph (`FnordView`)
Used with `'type' -> 'fnord'` or `'dual'` (e.g. `FnordApp`, `GrafAct`, `FnordFlight`, `FnordCannon`):
- **`GGl`** (passed to `app run: gl`):
  - Info & Touch: `wid`, `hei`, `frameCount`, `frameInterval`, `startTime`, `ex` (touch X scaled `0`..`100`), `ey` (touch Y scaled `0`..`100`, bottom-up), `esecs` (seconds since last touch).
  - Camera & Lighting (3-element `x@y@z` or `r@g@b` `Vec`s): `eye:`, `look:`, `up:`, `light:`, `ambient:`, `diffuse:`, `wires:` (`bool`).
  - Scene Posting: `post:` `rootNode` — atomically swaps the active scene-graph root rendered by the GL thread.
- **3D Scene Graph Nodes (`Node` $\rightarrow$ `DddNode`)**:
  - Transforms & Color on any `DddNode`: `pos:` / `pos` (`x@y@z`), `scale:` / `sca` (`sx@sy@sz`), `rot:` / `rot` (`rx@ry@rz` Euler degrees), `color:` / `color` (`r@g@b` or `r@g@b@a` floats `0.0`..`1.0`).
  - `Group`: `Group new`, `vec:` `vecOfNodes`, `vec`. Pushes transform/color matrix and renders children.
  - `Cube`: `Cube new` (unit cube $[-0.5, +0.5]^3$ with normals).
  - `Lines`: `Lines new:` `vecOf3DPoints` (`GL_LINE_STRIP`).
  - `Fan`, `Strip`: Stubs (`drawPrim` unimplemented).
- **4x4 Matrix Math (`Mat`)**:
  - `Mat new` (identity), `clone`, `vec` / `vec:` (16-float `Vec`), `mulM:` `otherMat`, `mulV:` `vec4`, `trans:` `xyz`, `scale:` `xyz`, `rot:` `rx@ry@rz`, `rot:axis:` `deg` `xyz`.
- **2D HUD Overlay Nodes (`Node` $\rightarrow$ `DdNode`)**:
  - Placed inside a 3D `Group` and automatically rendered on the 2D `OverView` canvas atop OpenGL:
  - `Print new:` `str` (auto-advancing HUD text lines), `Seg new:` `vecOf2DPoints`, `Rec new:` `vecOf2DPoints`.

### 7.4 Experimental Native Android Views
- `BaseViewPrim` (`main`), `ValViewPrim`, `TextValViewPrim` (`new`, `text:`), `LinearViewPrim` (`new`, `add:`): Early prototypes at the bottom of `TerseActivity.java` (and `Stashed-LinearV.diff`) for composable Android widgets.

### 7.5 Summary of `prelude.txt` Application Frameworks & Built-in Apps
- **Base Frameworks**:
  - `App` (subclass of `Usr`): Root of all URL-routable applications (`Top` is default `/` launcher).
  - `WebApp` / `HtApp`: Template-method HTML app base classes (`wTitle`, `wMid`, `wFootLinks`, navigation chrome).
  - `DrawApp`: Retained-mode 2D vector apps (`DemoStar`, `DemoLissajous`).
  - `LiveApp` & `Act` / `DrawAct`: Real-time 2D `Screen`/`Ink` apps (`DemoLiveLissajous`, `DemoLiveSierpinski`, `DemoLivePaint`, `DemoLiveLife`, `DemoLiveLifeRedBlue`, `GameFoo`, `GamePong`, `PongGame`, `GameTroids`).
  - `FnordApp` & `GrafAct`: Real-time 3D OpenGL ES apps (`BlocksDemo`, `FnordDebugRot`, `FnordCannon`, `FnordCheeseDemo`, `FnordTwo`, `FnordRandom`, `FnordMotherShip`, `FnordFlight` + `RedPlane`).
- **On-Device IDE & Tools**:
  - `BrowseClasses` / `BrowseClass` / `BrowseMethod` / `EditMethod` / `SubmitMethod` / `SubmitInstVars` / `FormCreateSubclass` (and older `Browse`): Full on-device Smalltalk-style class/method browser and editor.
  - `Eval` / `SubmitEval`: Interactive workspace ("DoIt / PrintIt").
  - `Inspect` / `InspectRoot` / `InspectClass` / `InspectInst`: Live object and class inspector.
  - `SaidWhat`: Viewer for the `Sys said` log ring buffer.
  - `BrowseFiles` / `BrowseFile` / `EditFile`, `BrowseHub`, `BrowseWorlds` / `SnapshotWorld` / `SwitchWorld`: Local file editor, world switcher, and remote Hub sync tools.
  - `ADozenYaksApp` (`NamedDy`, `FriendDy`, `SelfDy`, `RoomDy`, `MsgDy`): Peer-to-peer Diffie-Hellman + SHA-1 + `Pickle` encrypted messaging client.

---

## 8. Notable Quirks & Bugs in `TerseTalk-Alpha` (Notes for Re-implementation)

When re-implementing a TerseTalk-like language, keep the following Alpha quirks in mind:
1. **`Num` `ln`, `log10`, `exp` bug**: In `Ur.java` (lines 1451–1464), `_ln()`, `_log10()`, and `_exp()` were accidentally left as stubs returning `-num` instead of calling `Math.log`, `Math.log10`, and `Math.exp`.
2. **`Vec >> ==` vs `equals:`**: `prelude.txt` defines `Vec >> eq:` (aliased to `==` via `Obj >> ==`) to return an **elementwise `Vec` of `0`/`1`** and throw an error if lengths differ or the RHS is not a `Vec`, whereas `Ur >> equals:` performs standard boolean (`1`/`0`) structural equality.
3. **`Vec >> dot:`**: Computes elementwise multiplication returning a `Vec` rather than summing into a scalar dot product.
4. **Toroidal `Vec >> at:`**: `Vec >> at:` and `at:put:` wrap *all* out-of-bounds indices modulo `len` (not just `-len .. -1`). Several demos (e.g., `DemoLife` and `DemoLiveLife`) rely on this toroidal wrap-around!
5. **`vars` vs `defVars:`**: File-loaded `vars` directives auto-generate getter (`x`) and fluent setter (`x:`) methods; runtime `Cls >> defVars:` does not, and adding instance variables to a superclass at runtime does not recompile existing subclass methods whose field indices shift.
6. **Preemption via `tickCounter`**: `Terp` decrements `tickCounter` on every `Send` and `Seq`/`MakeVec` step, throwing `TooManyTicks` to preemptively kill infinite loops in background UI threads when switching screens.
7. **`JsonUtils.Quote` fallthrough & `Pickle` `Dict` bug**: `JsonUtils.Quote` is missing `break;` statements in its `switch` for escape characters, and `Pickle`'s `visitDict` iterates over the newly created empty `z.dict` instead of `a.dict`.
