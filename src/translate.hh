// Turning an `fznso::Model` into Z3 expressions.
//
// The whole of the translation is one idea: **a constraint is one Boolean
// formula over its arguments**. No auxiliary decision variable, no second
// assertion. An SMT solver's propagator is the formula, so `_reif` is `r == f`
// and `_imp` is `r -> f`, and both come free once the base form is written.
//
// Nothing here materialises the model. Values are read through the callback
// table as the formula is built, and every accessor is O(1).

#ifndef FZNSO_Z3_TRANSLATE_HH
#define FZNSO_Z3_TRANSLATE_HH

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <z3++.h>

#include "fznso.hpp"

namespace fznso_z3 {

/// What sort a `var float` decision takes.
///
/// The two are genuinely different constraints, not two implementations of one:
/// IEEE-754 addition is not associative, so under `Fpa` a linear sum means the
/// left-folded rounded chain, while under `Real` it means the exact sum every
/// other MiniZinc backend reasons about. Neither is wrong; they answer
/// differently, and the option is how a caller says which it wants.
enum class FloatEncoding : std::uint8_t {
	/// `(_ FloatingPoint 11 53)` — IEEE-754 double, rounding and all.
	Fpa,
	/// Z3 `Real` — exact rational arithmetic.
	Real,
};

/// How a decision's value is read back out of a model.
enum class Kind : std::uint8_t { Bool, Int, Float };

/// The model, as Z3 sees it.
///
/// One per solver instance, living as long as the Z3 context does: the decision
/// variables it holds are what a later layer's constraints refer to.
class Translate {
public:
	Translate(z3::context& ctx, FloatEncoding encoding);

	FloatEncoding encoding() const { return encoding_; }

	/// Create the decision `d` and return the constraint its domain imposes, or
	/// absent when the domain does not constrain it at all.
	///
	/// An absent domain is the ordinary case for this solver rather than an
	/// error: Z3's integers are unbounded, so a `var int` with no declared
	/// bounds simply gets no constraint. A domain with holes becomes a
	/// disjunction of intervals — one term per range, never one per value.
	std::optional<z3::expr> add_decision(const fznso::Model& model, fznso::Decision d);

	/// The constraint as one assertion, with `_reif` / `_imp` already applied.
	z3::expr assertion(const fznso::Model& model, fznso::Constraint c);

	/// An objective argument as a term an optimiser will accept.
	///
	/// Z3 optimises over integers and reals only, so under `Fpa` this is
	/// `fp.to_real(v)` — which is undefined on NaN and the infinities, so the
	/// side conditions ruling those out are appended to `guards`.
	z3::expr objective_term(fznso::Value v, z3::expr_vector& guards) const;

	/// The value assigned to decision `d`, as the interface carries it.
	fznso::OwnedValue read(const z3::model& m, std::size_t d) const;

	std::size_t size() const { return vars_.size(); }
	Kind kind(std::size_t d) const { return kinds_[d]; }
	const z3::expr& var(std::size_t d) const { return vars_[d]; }

	/// Forget the decisions from `n` onwards, which is what popping a layer
	/// means here: a Z3 constant survives a `pop` — only its assertions are
	/// retracted — so dropping our record of it is what actually forgets it.
	void truncate(std::size_t n);

	/// Forget everything and start again under `encoding`.
	///
	/// The encoding decides the sort of every float decision, so changing it
	/// cannot keep the variables it was holding.
	void reset(FloatEncoding encoding);

	/// Whether a nonlinear term over the reals has been built since the flag was
	/// last cleared.
	///
	/// This is not bookkeeping for its own sake. Z3's optimiser is **not sound
	/// for nonlinear real arithmetic**: on `x*x + y*y <= 16, maximize x + y` it
	/// reports an optimum of 723/128 where the true one is 4*sqrt(2), and says
	/// `lower == upper` as though it had proved it. The optimum of such a
	/// problem is often irrational and the optimiser works over the rationals,
	/// so it can only approach it. Nonlinear *integer* optimisation is fine —
	/// measured, not assumed — and so is nonlinear real *satisfaction*, since
	/// nlsat decides that. Only the claim of optimality is unsound, so only that
	/// is withheld.
	bool saw_nonlinear_real() const { return nonlinear_real_; }
	void clear_nonlinear_real() { nonlinear_real_ = false; }

	/// A clause ruling out the assignment `m` gives the decisions in `wanted`.
	///
	/// Enumerating solutions blocks only what the caller asked to see: two
	/// models differing on an auxiliary variable are the same answer, and
	/// blocking on one would report it twice.
	z3::expr blocking_clause(const z3::model& m, const std::vector<std::size_t>& wanted) const;

private:
	// --- reading arguments --------------------------------------------------

	z3::expr bool_arg(fznso::Value v) const;
	z3::expr int_arg(fznso::Value v) const;
	z3::expr float_arg(fznso::Value v) const;
	/// A parameter integer: a coefficient, an offset, a size.
	std::int64_t int_par(fznso::Value v) const;

	/// The argument list as expressions of the matching sort.
	z3::expr_vector bool_args(fznso::Value list) const;
	z3::expr_vector int_args(fznso::Value list) const;
	z3::expr_vector float_args(fznso::Value list) const;

	// --- float helpers, the two encodings' only real difference -------------

	z3::sort float_sort() const;
	z3::expr float_val(double d) const;
	/// Numeric equality. `operator==` builds a *structural* equality, which for
	/// FPA distinguishes `+0.0` from `-0.0` and identifies NaN with itself —
	/// neither of which is what a float constraint means.
	z3::expr float_eq(const z3::expr& a, const z3::expr& b) const;
	/// How a float is taken to an integer.
	enum class Rounding : std::uint8_t { Down, Up, NearestAway };

	/// The IEEE-754 rounding mode as a term.
	z3::expr rounding_mode(Rounding r) const;

	/// The integral part of `a`, rounded as `r` says, as an integer term.
	z3::expr float_to_integer(const z3::expr& a, Rounding r) const;

	/// Take ownership of a raw AST, which must happen **immediately**.
	///
	/// `z3::context` is reference-counted, so an AST that nothing has referenced
	/// yet is collectable the moment anything else drops a reference — and a
	/// temporary `z3::expr` dying at the end of the enclosing full-expression
	/// does exactly that. Nesting one `Z3_mk_*` call inside another therefore
	/// hands Z3 a pointer it may already have freed, which surfaces as an
	/// assertion failure deep inside `ast.cpp` rather than as anything to do
	/// with the call that caused it. Every raw call goes through here.
	z3::expr own(Z3_ast raw) const { return z3::expr(ctx_, raw); }
	z3::expr to_real(const z3::expr& a) const;

	// --- shared shapes ------------------------------------------------------

	/// `sum(coeffs[i] * xs[i])`, folded left to right. Coefficients of 0, 1 and
	/// -1 are folded away, which keeps the formula small without changing it.
	z3::expr linear(fznso::Value coeffs, const z3::expr_vector& xs, bool floating) const;
	/// The set as a disjunction of interval tests: `x in {1,3..5}` is
	/// `x = 1 \/ (3 <= x /\ x <= 5)`. One term per range, never one per value.
	z3::expr in_set(const z3::expr& x, fznso::Value set) const;
	/// `value = xs[index - offset]`, as a nested `ite` over `xs` together with
	/// the range constraint that makes the final `else` sound.
	z3::expr element(const z3::expr_vector& xs, std::int64_t offset, const z3::expr& index,
	                 const z3::expr& value, bool floating) const;
	/// `result` is the extremum of `xs`, as an `ite` chain.
	z3::expr extremum(const z3::expr_vector& xs, const z3::expr& result, bool maximum,
	                  bool floating) const;
	/// `xs` before `ys` in lexicographic order. The arrays need not have equal
	/// length; a proper prefix is smaller.
	z3::expr lex(const z3::expr_vector& xs, const z3::expr_vector& ys, bool strict,
	             bool floating) const;

	/// The base constraint as a formula, `_reif` / `_imp` not yet applied.
	z3::expr formula(std::string_view ident, const fznso::Model& model, fznso::Constraint c);

	z3::context& ctx_;
	FloatEncoding encoding_;
	std::vector<z3::expr> vars_;
	std::vector<Kind> kinds_;
	bool nonlinear_real_ = false;
};

} // namespace fznso_z3

#endif // FZNSO_Z3_TRANSLATE_HH
