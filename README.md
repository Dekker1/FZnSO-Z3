# Z3 behind FZnSO

An SMT backend for the FZnSO interface. MiniZinc has no SMT backend, so this is
new capability rather than a re-wiring of an existing one — and it was the first
solver behind the interface to exercise three things the ABI was designed for
and nothing had reached: **incremental layers**, **unbounded integers** and
**multi-objective search**. (The MIP backend has since grown layer support of
its own; when this was written it rebuilt from scratch every run.)

## The one idea

**A constraint is one Boolean formula over its arguments.** No auxiliary
decision variable, no second assertion. An SMT solver's propagator *is* the
formula, so the whole translation is a single function:

```cpp
z3::expr Translate::formula(std::string_view ident, const fznso::Model&, fznso::Constraint);
```

and the dispatcher wraps it:

| identifier ends in | asserted |
| --- | --- |
| — | `f` |
| `_reif` | `r == f` |
| `_imp` | `r -> f` |

That is why this solver declares 131 constraints where the MIP backend declares
nine: **reification is free here**, so every `_reif` and `_imp` form costs one
line in the declaration table and nothing in the poster. Declaring the `_imp`
forms is not decoration — MiniZinc derives the rewrite onto a half-reified
target only when the declared list contains it, and omitting one costs a reified
constraint plus an introduced Boolean plus a clause.

The line the declaration table is drawn on is exactly that: **one
quantifier-free formula, no introduced variable, no second constraint**. A
global that would need an auxiliary variable is left to the library however easy
it would be to write, because a declaration MiniZinc believes and the solver
cannot honour is worse than no declaration at all.

## Layers are Z3 assertion scopes

`layer_unchanged()` says how much of the model the solver has already seen. Z3
has assertion levels natively, so the mapping is direct: **one `Z3_optimize`
scope per model layer**, and the solver keeps that stack between runs.

```
keep = min( layer_unchanged(), min(posted_layers, layer_count()) )
pop  down to `keep`; post `keep .. layer_count()`
```

Only `layer_unchanged()` promises the solver has seen a layer as it is now.
`layer_permanent()` is no floor: a consumer can pop a layer, push a different
one and mark it permanent before the next run. Clamping by `posted_layers` and
`layer_count()` is what makes *popping* free: a model that shrank drops the
surplus scopes and posts nothing at all.

A Z3 constant outlives the scope its assertions were made in, so popping a scope
is not enough to forget a layer's decisions — the solver truncates its own
`vars_` too. Global indices follow layer order, which is what makes a truncation
correct.

Everything a *run* adds — the objective, its side conditions, the blocking
clauses of an enumeration — goes in one further scope pushed and popped around
the run, so none of it leaks into the layer state the next run reuses.

The `z3_layers_posted` statistic reports how many layers the last run had to
post. **Zero means the whole model was reused**, and that is the only direct
evidence that incrementality is working: a solver that quietly rebuilt
everything would give the same answers. The self-test asserts on it.

### What it is actually worth

Measured on 25 000 decisions and ~50 000 constraints, solved once and then
re-solved ten times with one more constraint each time — layers reused, against
the fresh instance a consumer without them would create:

| | posting | wall clock |
| --- | --- | --- |
| layers reused | **0.13s** | 18.90s |
| fresh instance each time | 1.39s | 19.95s |

**Translation cost drops by a factor of ten**, which is exactly the claim the
layer mechanism makes and the thing it is designed to remove. End to end the
win is only about 5%, because Z3 re-solves the formula from scratch on every
`check()` and search dominates here. So the honest summary is: layers deliver
what they promise — re-translation stops being paid for — and how much that is
worth depends entirely on a model's translation-to-search ratio. A consumer
that adds a constraint to a large model and re-solves cheaply is the case that
benefits; one whose search dwarfs its translation will barely notice.

To measure against a non-incremental baseline, `set_unchanged(0)` before each
run: every layer is rebuilt, permanent or not.

## Unbounded integers

`decision_domain` returning `FznsoValueAbsent` is the ordinary case here, not an
error: Z3's integers have no bounds, so an unbounded `var int` gets no
constraint at all. A domain that *is* present becomes a disjunction of interval
tests — one term per range, never one per value, so a domain with holes costs
nothing extra.

The reverse edge is real, though: Z3's integers are unbounded and this
interface's are `int64_t`. A solution value that does not fit is reported as an
error rather than truncated, because a wrapped value would be a wrong answer
that nothing downstream could detect.

## Objectives

All ten registry strategies, which nothing had previously declared in full:

| strategy | how |
| --- | --- |
| `int_minimize` / `int_maximize` | `optimize::minimize` / `maximize` |
| `float_minimize` / `float_maximize` | the same, on the objective key below |
| `int_lex_minimize` / `_maximize`, and the float forms | `priority=lex`, objectives added in list order |
| `int_pareto_maximize`, `float_pareto_maximize` | `priority=pareto`, then `check()` in a loop reporting each point until `unsat` |

Pareto search reports a *set* of incomparable solutions rather than converging
on one, so `intermediate` has no effect there and every point goes out through
`on_solution` as it is found — which is what the registry entry says.

An unbounded objective comes back from Z3 as `sat` with the objective reported
as `oo`. That is not an answer, so it is `FznsoError` with "the objective is
unbounded", as the MIP backend does.

## The two float encodings

`z3_float_encoding` takes `"float"` (the default) or `"real"`.

- **`float`** — `(_ FloatingPoint 11 53)`, genuine IEEE-754 doubles: rounding,
  signed zeros, infinities and all.
- **`real`** — Z3 `Real`, exact rational arithmetic, which is what Gecode
  (interval doubles) and a MIP backend (LP rationals) effectively reason about.

They are genuinely different constraints, not two implementations of one.
IEEE-754 addition is not associative, so under `float` a linear sum means the
left-folded rounded chain, in the order the arguments arrived, while under
`real` it means the exact sum. Neither is wrong; they answer differently, and
the option is how a caller says which it wants. Expect the two to disagree on
models where summation order matters — which is why the cross-solver comparison
against Gecode is run under `real`, where the two reason about the same thing.

Changing the option **drops the layer state**: it decides the sort of every
float decision, so the variables held from previous runs cannot survive it.

One declaration list serves both, which is what makes this an option rather than
two libraries. `float_sqrt` is the only entry that differs in kind — `fp.sqrt`
under `float`, and `b*b = a /\ b >= 0` under `real`, which is one formula over
the existing arguments and is unsatisfiable for a negative operand, exactly the
right failure.

### Why a float objective is not `fp.to_real`

Z3 optimises over integers, reals and bit-vectors only; a FloatingPoint
objective is rejected outright with *"Objective must be bit-vector, integer or
real"*. The obvious workaround, optimising `fp.to_real(f)`, is unusable: it puts
a bit-blasted FloatingPoint term inside the linear-arithmetic theory, and

```smt2
(declare-const f (_ FloatingPoint 11 53))
(assert (fp.leq ((_ to_fp 11 53) RNE 0.0) f))
(assert (fp.leq f ((_ to_fp 11 53) RNE 4.5)))
(maximize (fp.to_real f))
(check-sat)
```

does not finish. Not "is slow" — it was still running after twenty seconds on a
variable already pinned to a five-unit interval.

IEEE-754 was designed so that its values sort in the order of their bit patterns
read as sign-magnitude, so the objective rides an order-preserving 64-bit key
instead, which stays entirely inside the bit-vector theory the FloatingPoint
terms are already blasted into:

```
key(f) = to_ieee_bv(f), with the sign bit flipped when it is clear
                        and every bit flipped when it is set
```

Same answer, 0.17s instead of not finishing. The infinities need no special
case — they are the extremes of that order. NaN is not in the order at all, so
it is excluded; that is a constraint this solver adds of its own, and it says so
through the `warn` message scope.

## What is declared, and what is not

`cargo run -p fznso-conform -- check build/lib/fznso/Debug/libz3.dylib` holds
the declared list to the registry: 131 constraints, 10 objectives, all
conforming.

`fznso_constraints/` holds 104 files whose body is an `abort`, and they are two
very different things:

- **20 are the standard library's own refusals**, carried across verbatim —
  MiniZinc has never reified a `graph_path`, and its message says so. The base
  forms all decompose perfectly well; only the reified form is refused. Nothing
  to do, and nothing here is a regression.
- **84 say `"<ident>: this solver neither implements the constraint nor
  decomposes it"`.** A FlatZinc builtin backs those, so no body could exist and
  a model using one fails outright unless the solver declares it.

Every one of the 84 has a disposition, and `abort_coverage` in the self-test
re-derives the list from the library directory at test time rather than trusting
this table, so it cannot rot:

| how many | disposition |
| --- | --- |
| 17 `set_of_int_*` | shadowed by `fznso_nosets/`, which is on the include path because this solver declares no set decision type |
| ~48 int / bool / float primitives | declared natively, plus every `_reif` and `_imp` among them |
| `int_pow` | declared narrowed to a parameter exponent, which is what `redefinitions-2.2.1.mzn` actually emits |
| `float_sqrt` | declared; see above |
| 16 float transcendentals | **not supported.** `float_exp`, `float_ln`, the trig family: primitives with no theory in Z3 and no decomposition anywhere. There is nothing to write, and a MIP backend is in the same position |
| `float_pow` | **not supported.** The registry types its exponent `var float`; an integer exponent is a different type rather than a narrowing of one, and a genuine float power exists only over the reals and not at all over FloatingPoint, so one declaration cannot serve both encodings honestly |
| `int_alternative` (+`_reif`) | **not supported.** Optional-task scheduling needs introduced variables, which is library work |

`int_span` is undeclared for a different reason: it has no decomposition either,
but its extrema range over the *present* sub-tasks and the arguments it is
declared with carry no `opt` flag, so there is no way to tell a present task
from an absent one through the interface. Nothing in MiniZinc lowers to it yet.

Everything else — `int_nvalue`, the `int_global_cardinality` family,
`int_inverse`, the rest of scheduling, the whole graph family, every set
constraint — decomposes through `fznso_constraints/` and `std`, as it does for
the MIP backend. Where Z3 has no native form and we have no better
decomposition, the right answer is to leave the MiniZinc decomposition alone.

## No `mznlib`

Deliberately. A body in a solver's own MiniZinc library is **not** dropped by
`enable_native_predicates`, so it is the one place a rewrite loop can hide — and
a loop does not announce itself, the flattener simply never returns. A
hand-written decomposition that merely restates the standard library's is pure
risk for no gain.

## What this backend found

Two were in `share/minizinc/fznso_nosets/`, which had never been exercised by a
solver that has no `mznlib` of its own or that declares reified forms natively.
The third is an interaction with MiniZinc's half-reification derivation.

**`fzn_set_of_int_lt_imp.mzn` included `mip_linear.mzn`** — a file that exists
only in the MIP backend's own library. Every model failed to compile, for any
solver without that file, because `redefinitions.mzn` pulls the file in
unconditionally. HiGHS masked it by supplying `mip_linear.mzn` itself. Fixed by
stating that branch in plain MiniZinc, exactly as the reified form beside it
already did.

**`fzn_int_in_reif.mzn` made a partial array access inside a reification.**
`std/nosets.mzn` writes `b <-> set2bools(values)[x]`, and the access is partial
whenever `x` ranges wider than the set. MiniZinc totalises that into one reified
equality per value of `x`, each of which the overlay rewrites onto
`int_lin_eq_reif` — and for a solver that implements *that* natively the
rewriting does not terminate. `-Glinear` never sees it because it decomposes
`int_lin_eq_reif` too, and Gecode never sees it because it has set variables and
so is never on this include path. The root-context `fzn_int_in.mzn` beside it
already avoided the same partiality by narrowing `x` with leading conjuncts,
which only works in a root context; the reified form now pins a copy of the
index inside the array's index set instead.

**A raw `Z3_ast` must be wrapped the instant it is created.** `z3::context` is
reference-counted, so an AST that nothing has referenced yet is collectable the
moment anything else drops a reference — and a temporary `z3::expr` dying at the
end of the enclosing full-expression does exactly that. Nesting one `Z3_mk_*`
call inside another therefore hands Z3 a pointer it may already have freed. It
surfaced as `ASSERTION VIOLATION ... ast.cpp:375 UNEXPECTED CODE WAS REACHED`
from inside `int_to_float`, and the identical calls written with named locals
were fine. Every raw call now goes through one `own()` helper; there are no
nested `Z3_mk_*` calls left.

**Declaring `bool_array_member_imp` makes every model that uses it fail to
typecheck.** MiniZinc recognises a reified predicate by its *shape* — a scalar
`var bool` as the last parameter — rather than by a `_reif` suffix, because
`array_bool_or` and `bool_or` are reified forms without one
(`native_predicates.cpp:280`). `bool_array_member(xs, value)` is the only
non-functional registry constraint whose last argument is a scalar `var bool`,
so the rewrite onto it is read as *already reified on `value`*, and declaring
the `_imp` form is exactly what satisfies the guard that lets the flattener
emit a two-argument `fzn_bool_array_member_imp` to match. That collides with the
three-argument form the registry defines, and the error names a signature no
one wrote. The reified form is unaffected; only the derived half-reified one is
dropped here.

**`int_seq_precede_chain`'s registry formula was wrong, and is now fixed.**
`ordering.toml` defined it as `∀j : xs_j > 0 → ∃i < j : xs_i = xs_j − 1`, which
refuses `[1, 2, 3]` because the leading 1 has no 0 before it.
`fznso_constraints/fzn_int_seq_precede_chain.mzn` encodes a running maximum that
may climb by one at a time from an implicit floor of zero, which accepts it — and
so does MiniZinc, whose documentation reads "requires that i precedes i+1 for all
*positive* i". The bound is `> 1`; the registry now says so, for the set form
too. The two readings are otherwise equivalent, which is why nothing else caught
it: `∀j : xs_j ≤ 1 + max(0, xs_{<j})` and `∀j : xs_j > 1 → ∃i<j : xs_i = xs_j − 1`
agree on every array, and this backend posts the first.

**Z3's optimiser is not sound over nonlinear real arithmetic.** On
`x*x + y*y <= 16, maximize x + y` it answers 723/128 where the true optimum is
`4*sqrt(2)`, and reports `lower == upper` as though it had proved it. The
optimum of such a problem is often irrational and the optimiser works over the
rationals. `Z3_optimize_get_reason_unknown` is no help — it says `"unknown"` for
plain linear real maximisation too. So the translator tracks whether a nonlinear
real term was built, per layer, and a run with an objective over one returns
`Incomplete` rather than `Complete`: the solutions are valid, but the claim that
the last one is optimal is withheld. Nonlinear *integer* optimisation is
unaffected — measured against brute force, not assumed — and so is nonlinear real
*satisfaction*, which nlsat decides.

## Where it stands

Measured against Gecode over the 1088 models in `tests/spec/unit`, with
`z3_float_encoding=real` so that both sides reason about the same arithmetic.

| gate | result |
| --- | --- |
| self-test (`ctest`) | 21 groups, including layers, `mark_permanent`/`mark_redundant`, lexicographic and Pareto objectives, unbounded integers, and both float encodings |
| `fznso-conform` | 131 constraints, 10 objectives — all conform |
| compile sweep | **14** models only this path fails, **0** without a diagnostic. All 14 are `on_restart` or `blackbox`; HiGHS fails on the same 14 |
| `check-semantics` | **220 passed, 0 differing** |
| solutions vs Gecode | identical 688, equivalent 138, **differing 21**, skipped 241 |

Every one of the 21 is accounted for, and **none is a wrong answer**:

- **12** are the `on_restart` and `blackbox` features above.
- **4** are models where *Gecode* is the one that disagrees with the model's own
  expected result: `test_fldiv_02` and `test_set_lt_3` match their `!Test` block
  under Z3 and not under Gecode (and `test_set_lt_3`'s `solvers:` list excludes
  Gecode deliberately); `bug222` is solved here and `UNKNOWN` there; and
  `bug318_orig` minimises an unbounded `var int`, so "the objective is unbounded"
  is the truth and Gecode's `-2147483646` is an artefact of a finite domain.
- **3** are the nonlinear-real optimality claim being withheld on purpose — the
  answers match their expected results, only the `==========` marker is absent.
- **2** are timeouts on nonlinear or very large models.

The self-test is the only gate that reaches layers, multi-objective search and
unbounded integers at all, because no MiniZinc model can produce them.

## Building

```
cmake -S . -B build
cmake --build build
ctest --test-dir build -C Debug --output-on-failure
```

Z3 is linked in, not loaded at run time: an installed one is used if
`find_package(Z3 CONFIG)` finds it (`brew install z3`), and otherwise it is
fetched and built here.

The FZnSO name of a solver is its library's base name, so the result is
`build/lib/fznso/<config>/libz3.dylib` — the same leaf as Z3's own shared
library. The `fznso` directory keeps them apart; Z3's install name is
`@rpath/libz3.5.1.dylib`, a different leaf, so there is no self-reference.

## Options

`all_solutions`, `intermediate`, `time_limit`, `random_seed`, `verbose`, and
`z3_float_encoding`.

`intermediate` streams each improving solution as it is found. `optimize::check`
only ever hands back the last one, so this rides
`Z3_optimize_register_model_eh` — which has no C++ wrapper in z3++ 5.1.0 and so
is called through the C API directly. The final solution is not reported twice:
the last streamed one *is* the optimum.

**`threads` is deliberately not declared.** Z3's optimiser does not search on
several threads, and the registry entry is a claim that it could; declaring it
would mean accepting a number and ignoring it. MiniZinc's `-p` is then dropped
silently, which is only advice lost.

## Known limits

- Solution enumeration is a blocking-clause loop, so it is quadratic in the
  number of solutions. Fine for the single answer MiniZinc asks for by default,
  and for the `all_solutions` runs a test suite makes.
- Under the FloatingPoint encoding, float arithmetic is bit-blasted and models
  with more than a handful of float variables get slow. That is inherent to the
  theory, not to this backend; `z3_float_encoding=real` is the way out.
