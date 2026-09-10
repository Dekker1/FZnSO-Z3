#include "translate.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace fznso_z3 {

namespace {

/// The identifier a constraint dispatches on.
///
/// A declared name is registered twice by MiniZinc, as `fzn_<ident>` and bare
/// `<ident>`, because a declaration has to disable both the global's lowering
/// and the builtin's decomposition. The prefix records where the call came from
/// rather than what the constraint is, so it is dropped here.
std::string_view base_ident(std::string_view ident) {
	constexpr std::string_view kPrefix = "fzn_";
	return ident.compare(0, kPrefix.size(), kPrefix) == 0 ? ident.substr(kPrefix.size()) : ident;
}

/// Whether `ident` ends in `suffix`, and what is left when it does.
bool strip(std::string_view& ident, std::string_view suffix) {
	if (ident.size() <= suffix.size() ||
	    ident.compare(ident.size() - suffix.size(), suffix.size(), suffix) != 0) {
		return false;
	}
	ident.remove_suffix(suffix.size());
	return true;
}

[[noreturn]] void fail(std::string_view ident, const std::string& why) {
	throw std::runtime_error(std::string{ident} + ": " + why);
}

} // namespace

Translate::Translate(z3::context& ctx, FloatEncoding encoding) : ctx_(ctx), encoding_(encoding) {}

void Translate::truncate(std::size_t n) {
	if (n < vars_.size()) {
		vars_.erase(vars_.begin() + static_cast<std::ptrdiff_t>(n), vars_.end());
		kinds_.erase(kinds_.begin() + static_cast<std::ptrdiff_t>(n), kinds_.end());
	}
}

void Translate::reset(FloatEncoding encoding) {
	encoding_ = encoding;
	nonlinear_real_ = false;
	vars_.clear();
	kinds_.clear();
}

z3::expr Translate::blocking_clause(const z3::model& m,
                                    const std::vector<std::size_t>& wanted) const {
	z3::expr_vector differs(ctx_);
	for (std::size_t d : wanted) {
		if (d >= vars_.size()) {
			continue;
		}
		z3::expr value = m.eval(vars_[d], true);
		differs.push_back(kinds_[d] == Kind::Float ? !float_eq(vars_[d], value)
		                                           : z3::expr(vars_[d] != value));
	}
	return z3::mk_or(differs);
}

// ---------------------------------------------------------------------------
// Sorts and float helpers
// ---------------------------------------------------------------------------

z3::sort Translate::float_sort() const {
	return encoding_ == FloatEncoding::Fpa ? ctx_.fpa_sort<64>() : ctx_.real_sort();
}

z3::expr Translate::float_val(double d) const {
	if (encoding_ == FloatEncoding::Fpa) {
		return ctx_.fpa_val(d);
	}
	// A decimal string, not a `double` reinterpreted: Z3 parses it into the
	// exact rational the literal denotes. 17 significant digits round-trip a
	// binary64 value, so nothing is lost on the way in.
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%.17g", d);
	return ctx_.real_val(buffer);
}

z3::expr Translate::float_eq(const z3::expr& a, const z3::expr& b) const {
	if (encoding_ != FloatEncoding::Fpa) {
		return a == b;
	}
	// `operator==` would build a structural equality, which for floating point
	// says `+0.0` differs from `-0.0` and that NaN equals itself. Neither is
	// what a float constraint means, so numeric equality is asked for by name.
	return own(Z3_mk_fpa_eq(ctx_, a, b));
}

z3::expr Translate::to_real(const z3::expr& a) const {
	if (encoding_ != FloatEncoding::Fpa) {
		return a;
	}
	return own(Z3_mk_fpa_to_real(ctx_, a));
}

z3::expr Translate::rounding_mode(Rounding r) const {
	switch (r) {
	case Rounding::Down:
		return own(Z3_mk_fpa_rtn(ctx_));
	case Rounding::Up:
		return own(Z3_mk_fpa_rtp(ctx_));
	case Rounding::NearestAway:
		break;
	}
	return own(Z3_mk_fpa_rna(ctx_));
}

z3::expr Translate::float_to_integer(const z3::expr& a, Rounding r) const {
	if (encoding_ != FloatEncoding::Fpa) {
		// `real2int` is the floor, so the other two are stated in terms of it.
		if (r == Rounding::Down) {
			return own(Z3_mk_real2int(ctx_, a));
		}
		if (r == Rounding::Up) {
			z3::expr negated = -a;
			return -own(Z3_mk_real2int(ctx_, negated));
		}
		// Ties away from zero, which is what MiniZinc's `round` does.
		z3::expr half = float_val(0.5);
		z3::expr up = a + half;
		z3::expr down = -a + half;
		return z3::ite(a >= float_val(0.0), own(Z3_mk_real2int(ctx_, up)),
		               -own(Z3_mk_real2int(ctx_, down)));
	}
	// Round to an integral float first, then cross into the integers exactly.
	z3::expr mode = rounding_mode(r);
	z3::expr integral = own(Z3_mk_fpa_round_to_integral(ctx_, mode, a));
	z3::expr as_real = own(Z3_mk_fpa_to_real(ctx_, integral));
	return own(Z3_mk_real2int(ctx_, as_real));
}

// ---------------------------------------------------------------------------
// Decisions
// ---------------------------------------------------------------------------

std::optional<z3::expr> Translate::add_decision(const fznso::Model& model, fznso::Decision d) {
	FznsoType type = model.decision_type(d);
	if (type.set_of || type.list_of) {
		throw std::runtime_error("decision " + std::to_string(d.index) +
		                         ": this solver has no set or list variables");
	}

	// The name is for debugging only — it may be absent, duplicated or
	// meaningless — so the Z3 constant is named from the index, which is what
	// actually identifies a variable across this interface. A model's own name
	// is appended when there is one, purely so that `(get-model)` output during
	// a debugging session is readable.
	std::string name = "x" + std::to_string(d.index);
	if (std::optional<std::string_view> given = model.decision_name(d); given.has_value()) {
		name += "_";
		name.append(*given);
	}

	Kind kind = Kind::Int;
	switch (type.base) {
	case FznsoTypeBaseBool:
		kind = Kind::Bool;
		vars_.push_back(ctx_.bool_const(name.c_str()));
		break;
	case FznsoTypeBaseInt:
		kind = Kind::Int;
		vars_.push_back(ctx_.int_const(name.c_str()));
		break;
	case FznsoTypeBaseFloat:
		kind = Kind::Float;
		vars_.push_back(ctx_.constant(name.c_str(), float_sort()));
		break;
	default:
		throw std::runtime_error("decision " + std::to_string(d.index) + ": unsupported type");
	}
	kinds_.push_back(kind);

	const z3::expr& x = vars_.back();
	fznso::Value domain = model.decision_domain(d);
	if (kind == Kind::Bool) {
		// A Boolean's domain says nothing a `Bool` does not already say, except
		// when it fixes the variable — which arrives as a one-element set.
		return std::nullopt;
	}
	if (domain.kind() != FznsoValueSetInt && domain.kind() != FznsoValueSetFloat) {
		// No domain at all. This is the ordinary case here rather than an
		// error: Z3's integers and reals are unbounded, so there is simply
		// nothing to assert.
		return std::nullopt;
	}
	return in_set(x, domain);
}

// ---------------------------------------------------------------------------
// Reading arguments
// ---------------------------------------------------------------------------

z3::expr Translate::bool_arg(fznso::Value v) const {
	switch (v.kind()) {
	case FznsoValueBool:
		return ctx_.bool_val(v.as_bool());
	case FznsoValueDecision: {
		std::size_t i = v.as_decision().index;
		if (i >= vars_.size() || kinds_[i] != Kind::Bool) {
			throw std::runtime_error("expected a Boolean decision, got decision " +
			                         std::to_string(i));
		}
		return vars_[i];
	}
	default:
		throw std::runtime_error("expected a Boolean argument");
	}
}

z3::expr Translate::int_arg(fznso::Value v) const {
	switch (v.kind()) {
	case FznsoValueInt:
		return ctx_.int_val(static_cast<std::int64_t>(v.as_int()));
	case FznsoValueBool:
		return ctx_.int_val(v.as_bool() ? 1 : 0);
	case FznsoValueDecision: {
		std::size_t i = v.as_decision().index;
		if (i >= vars_.size() || kinds_[i] != Kind::Int) {
			throw std::runtime_error("expected an integer decision, got decision " +
			                         std::to_string(i));
		}
		return vars_[i];
	}
	default:
		throw std::runtime_error("expected an integer argument");
	}
}

z3::expr Translate::float_arg(fznso::Value v) const {
	switch (v.kind()) {
	case FznsoValueFloat:
		return float_val(v.as_float());
	case FznsoValueInt:
		// A literal in a float position may still arrive typed as an integer.
		return float_val(static_cast<double>(v.as_int()));
	case FznsoValueDecision: {
		std::size_t i = v.as_decision().index;
		if (i >= vars_.size() || kinds_[i] != Kind::Float) {
			throw std::runtime_error("expected a float decision, got decision " +
			                         std::to_string(i));
		}
		return vars_[i];
	}
	default:
		throw std::runtime_error("expected a float argument");
	}
}

std::int64_t Translate::int_par(fznso::Value v) const {
	switch (v.kind()) {
	case FznsoValueInt:
		return v.as_int();
	case FznsoValueBool:
		return v.as_bool() ? 1 : 0;
	default:
		throw std::runtime_error("expected a parameter integer");
	}
}

z3::expr_vector Translate::bool_args(fznso::Value list) const {
	z3::expr_vector out(ctx_);
	for (std::size_t i = 0; i < list.size(); i++) {
		out.push_back(bool_arg(list[i]));
	}
	return out;
}

z3::expr_vector Translate::int_args(fznso::Value list) const {
	z3::expr_vector out(ctx_);
	for (std::size_t i = 0; i < list.size(); i++) {
		out.push_back(int_arg(list[i]));
	}
	return out;
}

z3::expr_vector Translate::float_args(fznso::Value list) const {
	z3::expr_vector out(ctx_);
	for (std::size_t i = 0; i < list.size(); i++) {
		out.push_back(float_arg(list[i]));
	}
	return out;
}

// ---------------------------------------------------------------------------
// Shared shapes
// ---------------------------------------------------------------------------

z3::expr Translate::linear(fznso::Value coeffs, const z3::expr_vector& xs, bool floating) const {
	if (coeffs.size() != xs.size()) {
		throw std::runtime_error("a linear constraint's coefficients and variables differ in "
		                         "length");
	}
	z3::expr_vector terms(ctx_);
	for (unsigned i = 0; i < xs.size(); i++) {
		fznso::Value c = coeffs[i];
		if (floating) {
			double a = c.kind() == FznsoValueFloat ? c.as_float() : static_cast<double>(c.as_int());
			if (a == 0.0) {
				continue;
			}
			terms.push_back(a == 1.0 ? xs[i] : float_val(a) * xs[i]);
			continue;
		}
		std::int64_t a = int_par(c);
		if (a == 0) {
			continue;
		}
		// Folding 1 and -1 away is not an optimisation of the constraint, only
		// of the term: a smaller formula is a smaller thing for Z3 to simplify.
		terms.push_back(a == 1 ? xs[i] : a == -1 ? -xs[i] : ctx_.int_val(a) * xs[i]);
	}
	if (terms.empty()) {
		return floating ? float_val(0.0) : ctx_.int_val(0);
	}
	if (terms.size() == 1) {
		return terms[0];
	}
	if (!floating) {
		return z3::sum(terms);
	}
	// Under `Fpa` the fold order is the constraint, not an implementation
	// detail: IEEE-754 addition is not associative, so left to right in the
	// order the arguments arrived is the only defensible reading.
	z3::expr total = terms[0];
	for (unsigned i = 1; i < terms.size(); i++) {
		total = total + terms[i];
	}
	return total;
}

z3::expr Translate::in_set(const z3::expr& x, fznso::Value set) const {
	bool floating = set.kind() == FznsoValueSetFloat;
	std::size_t n = set.size();
	if (n == 0) {
		// An explicitly empty domain: nothing satisfies it.
		return ctx_.bool_val(false);
	}
	z3::expr_vector ranges(ctx_);
	for (std::size_t i = 0; i < n; i++) {
		if (floating) {
			fznso::Range<double> r = set.float_range(i);
			ranges.push_back(float_val(r.min) <= x && x <= float_val(r.max));
		} else {
			fznso::Range<std::int64_t> r = set.int_range(i);
			if (r.min == r.max) {
				ranges.push_back(x == ctx_.int_val(r.min));
			} else {
				ranges.push_back(ctx_.int_val(r.min) <= x && x <= ctx_.int_val(r.max));
			}
		}
	}
	return z3::mk_or(ranges);
}

z3::expr Translate::element(const z3::expr_vector& xs, std::int64_t offset, const z3::expr& index,
                            const z3::expr& value, bool floating) const {
	auto n = static_cast<std::int64_t>(xs.size());
	if (n == 0) {
		return ctx_.bool_val(false);
	}
	// Built from the back, so the innermost `else` is the last element. That
	// final `else` is only sound because the index is pinned to the array's
	// range by the conjunct below.
	z3::expr chain = xs[static_cast<unsigned>(n - 1)];
	for (std::int64_t k = n - 2; k >= 0; k--) {
		chain = z3::ite(index == ctx_.int_val(offset + k), xs[static_cast<unsigned>(k)], chain);
	}
	z3::expr bounded = ctx_.int_val(offset) <= index && index <= ctx_.int_val(offset + n - 1);
	return bounded && (floating ? float_eq(value, chain) : value == chain);
}

z3::expr Translate::extremum(const z3::expr_vector& xs, const z3::expr& result, bool maximum,
                             bool floating) const {
	if (xs.empty()) {
		return ctx_.bool_val(false);
	}
	z3::expr best = xs[0];
	for (unsigned i = 1; i < xs.size(); i++) {
		best = z3::ite(maximum ? xs[i] > best : xs[i] < best, xs[i], best);
	}
	return floating ? float_eq(result, best) : result == best;
}

z3::expr Translate::lex(const z3::expr_vector& xs, const z3::expr_vector& ys, bool strict,
                        bool floating) const {
	unsigned n = std::min(xs.size(), ys.size());
	// Running off the end of the shorter array: a proper prefix is smaller, so
	// what is left is a comparison of the lengths.
	bool prefix_ok = strict ? xs.size() < ys.size() : xs.size() <= ys.size();
	z3::expr chain = ctx_.bool_val(prefix_ok);
	// `==` is an equivalence for Booleans and an equality for integers; only
	// floating point needs it asked for by name.
	auto equal = [&](unsigned i) { return floating ? float_eq(xs[i], ys[i]) : xs[i] == ys[i]; };
	auto below = [&](unsigned i) {
		// `false < true`, so for Booleans "strictly below" is `!x /\ y`.
		return xs[i].is_bool() ? (!xs[i] && ys[i]) : z3::expr(xs[i] < ys[i]);
	};
	for (unsigned k = n; k > 0; k--) {
		unsigned i = k - 1;
		chain = below(i) || (equal(i) && chain);
	}
	return chain;
}

// ---------------------------------------------------------------------------
// The constraints
// ---------------------------------------------------------------------------

z3::expr Translate::assertion(const fznso::Model& model, fznso::Constraint c) {
	std::string_view ident = base_ident(model.constraint_ident(c));

	// `_reif` and `_imp` are not separate constraints here. The base form is a
	// Boolean formula, so reifying it is an equivalence and half-reifying it an
	// implication — which is the whole reason this solver declares them at all.
	bool reified = false;
	bool implied = false;
	std::string_view base = ident;
	if (strip(base, "_reif")) {
		reified = true;
	} else if (strip(base, "_imp")) {
		implied = true;
	}

	if (!reified && !implied) {
		return formula(base, model, c);
	}

	std::size_t argc = model.constraint_argument_count(c);
	if (argc == 0) {
		fail(ident, "a reified constraint takes a Boolean as its last argument");
	}
	z3::expr r = bool_arg(model.constraint_argument(c, argc - 1));
	z3::expr f = formula(base, model, c);
	return reified ? (r == f) : z3::implies(r, f);
}

z3::expr Translate::formula(std::string_view ident, const fznso::Model& model,
                            fznso::Constraint c) {
	std::size_t argc = model.constraint_argument_count(c);
	auto arg = [&](std::size_t i) {
		if (i >= argc) {
			fail(ident, "expected more arguments than the " + std::to_string(argc) + " given");
		}
		return model.constraint_argument(c, i);
	};
	auto want = [&](std::size_t n) {
		// A reified form carries one extra argument, so the check is a floor.
		if (argc < n) {
			fail(ident, "expected " + std::to_string(n) + " arguments, got " +
			                std::to_string(argc));
		}
	};
	auto count_of = [&](const z3::expr_vector& xs, auto matches) {
		z3::expr_vector terms(ctx_);
		for (unsigned i = 0; i < xs.size(); i++) {
			terms.push_back(z3::ite(matches(i), ctx_.int_val(1), ctx_.int_val(0)));
		}
		return terms.empty() ? ctx_.int_val(0) : z3::sum(terms);
	};

	// --- integer linear -----------------------------------------------------
	if (ident == "int_lin_eq" || ident == "int_lin_le" || ident == "int_lin_ne") {
		want(3);
		z3::expr lhs = linear(arg(0), int_args(arg(1)), false);
		z3::expr rhs = int_arg(arg(2));
		return ident == "int_lin_eq" ? lhs == rhs : ident == "int_lin_le" ? lhs <= rhs : lhs != rhs;
	}

	// --- integer arithmetic -------------------------------------------------
	if (ident == "int_abs") {
		want(2);
		return int_arg(arg(1)) == z3::abs(int_arg(arg(0)));
	}
	if (ident == "int_times") {
		want(3);
		return int_arg(arg(2)) == int_arg(arg(0)) * int_arg(arg(1));
	}
	if (ident == "int_div" || ident == "int_mod") {
		want(3);
		z3::expr a = int_arg(arg(0));
		z3::expr b = int_arg(arg(1));
		// SMT-LIB integer division is Euclidean: `(div -7 2)` is -4, where
		// MiniZinc truncates toward zero and gives -3. Correcting the quotient
		// is a term, not a reformulation — no variable is introduced and no
		// second constraint is posted.
		z3::expr euclid = a / b;
		z3::expr remainder = z3::mod(a, b);
		z3::expr quotient =
			z3::ite(a >= 0 || remainder == 0, euclid,
		            euclid + z3::ite(b > 0, ctx_.int_val(1), ctx_.int_val(-1)));
		// Division by zero is total and uninterpreted in Z3, but must fail in
		// FlatZinc, so it is ruled out as part of the same assertion.
		z3::expr defined = b != 0;
		return defined && (ident == "int_div" ? int_arg(arg(2)) == quotient
		                                      : int_arg(arg(2)) == a - b * quotient);
	}
	if (ident == "int_pow") {
		want(3);
		z3::expr a = int_arg(arg(0));
		std::int64_t e = int_par(arg(1));
		if (e < 0) {
			// Declared narrowed to a parameter exponent, and a negative one has
			// no integer answer except for a base of 1 or -1. Refusing is
			// better than encoding that special case as if it were arithmetic.
			fail(ident, "a negative exponent has no integer power");
		}
		z3::expr power = ctx_.int_val(1);
		for (std::int64_t k = 0; k < e; k++) {
			power = power * a;
		}
		return int_arg(arg(2)) == power;
	}
	if (ident == "int_to_float") {
		want(2);
		z3::expr integer = int_arg(arg(0));
		z3::expr as_real = own(Z3_mk_int2real(ctx_, integer));
		if (encoding_ != FloatEncoding::Fpa) {
			return float_arg(arg(1)) == as_real;
		}
		z3::expr rne = own(Z3_mk_fpa_rne(ctx_));
		z3::sort fs = float_sort();
		z3::expr rounded = own(Z3_mk_fpa_to_fp_real(ctx_, rne, as_real, fs));
		return float_eq(float_arg(arg(1)), rounded);
	}

	// --- Booleans -----------------------------------------------------------
	if (ident == "bool_clause") {
		want(2);
		z3::expr_vector literals = bool_args(arg(0));
		z3::expr_vector negative = bool_args(arg(1));
		for (unsigned i = 0; i < negative.size(); i++) {
			literals.push_back(!negative[i]);
		}
		return z3::mk_or(literals);
	}
	if (ident == "bool_array_and") {
		want(2);
		return bool_arg(arg(1)) == z3::mk_and(bool_args(arg(0)));
	}
	if (ident == "bool_array_xor") {
		want(1);
		z3::expr_vector xs = bool_args(arg(0));
		// An odd number of them hold.
		z3::expr odd = ctx_.bool_val(false);
		for (unsigned i = 0; i < xs.size(); i++) {
			odd = odd ^ xs[i];
		}
		return odd;
	}
	if (ident == "bool_lin_eq" || ident == "bool_lin_le" || ident == "bool_lin_ne") {
		want(3);
		// A Boolean weighted sum: the Booleans enter the arithmetic as 0/1
		// terms, which is the one place a `var bool` genuinely becomes a number.
		z3::expr_vector xs = bool_args(arg(1));
		z3::expr_vector as_int(ctx_);
		for (unsigned i = 0; i < xs.size(); i++) {
			as_int.push_back(z3::ite(xs[i], ctx_.int_val(1), ctx_.int_val(0)));
		}
		z3::expr lhs = linear(arg(0), as_int, false);
		z3::expr rhs = int_arg(arg(2));
		return ident == "bool_lin_eq" ? lhs == rhs
		     : ident == "bool_lin_le" ? lhs <= rhs
		                              : lhs != rhs;
	}
	if (ident == "bool_to_int") {
		want(2);
		return int_arg(arg(1)) == z3::ite(bool_arg(arg(0)), ctx_.int_val(1), ctx_.int_val(0));
	}

	// --- float linear -------------------------------------------------------
	if (ident == "float_lin_eq" || ident == "float_lin_le" || ident == "float_lin_lt" ||
	    ident == "float_lin_ne") {
		want(3);
		z3::expr lhs = linear(arg(0), float_args(arg(1)), true);
		z3::expr rhs = float_arg(arg(2));
		if (ident == "float_lin_eq") {
			return float_eq(lhs, rhs);
		}
		if (ident == "float_lin_ne") {
			return !float_eq(lhs, rhs);
		}
		return ident == "float_lin_le" ? lhs <= rhs : lhs < rhs;
	}

	// --- float arithmetic ---------------------------------------------------
	if (ident == "float_abs") {
		want(2);
		return float_eq(float_arg(arg(1)), z3::abs(float_arg(arg(0))));
	}
	if (ident == "float_times") {
		want(3);
		z3::expr a = float_arg(arg(0));
		z3::expr b = float_arg(arg(1));
		if (encoding_ == FloatEncoding::Real && !a.is_numeral() && !b.is_numeral()) {
			nonlinear_real_ = true;
		}
		return float_eq(float_arg(arg(2)), a * b);
	}
	if (ident == "float_div") {
		want(3);
		z3::expr b = float_arg(arg(1));
		if (encoding_ == FloatEncoding::Real && !b.is_numeral()) {
			nonlinear_real_ = true;
		}
		return !float_eq(b, float_val(0.0)) && float_eq(float_arg(arg(2)), float_arg(arg(0)) / b);
	}
	if (ident == "float_sqrt") {
		want(2);
		z3::expr a = float_arg(arg(0));
		z3::expr b = float_arg(arg(1));
		if (encoding_ == FloatEncoding::Fpa) {
			z3::expr rne = own(Z3_mk_fpa_rne(ctx_));
			return float_eq(b, own(Z3_mk_fpa_sqrt(ctx_, rne, a)));
		}
		nonlinear_real_ = true;
		// Z3 has no square root over the reals, but the defining property is
		// itself one formula over the two arguments — no variable introduced,
		// no second constraint — and it is unsatisfiable for a negative `a`,
		// which is the right failure.
		return b >= float_val(0.0) && b * b == a;
	}
	if (ident == "float_floor" || ident == "float_ceil" || ident == "float_round") {
		want(2);
		z3::expr a = float_arg(arg(0));
		Rounding r = ident == "float_floor" ? Rounding::Down
		           : ident == "float_ceil"  ? Rounding::Up
		                                    : Rounding::NearestAway;
		z3::expr result = int_arg(arg(1)) == float_to_integer(a, r);
		if (encoding_ != FloatEncoding::Fpa) {
			return result;
		}
		// There is no integer for NaN or an infinity to be rounded to, and
		// `fp.to_real` says nothing about them, so they are ruled out here.
		z3::expr is_nan = own(Z3_mk_fpa_is_nan(ctx_, a));
		z3::expr is_inf = own(Z3_mk_fpa_is_infinite(ctx_, a));
		return !is_nan && !is_inf && result;
	}
	if (ident == "float_in") {
		want(3);
		z3::expr x = float_arg(arg(0));
		return float_arg(arg(1)) <= x && x <= float_arg(arg(2));
	}

	// --- element ------------------------------------------------------------
	if (ident == "int_array_element" || ident == "bool_array_element" ||
	    ident == "float_array_element") {
		want(4);
		bool floating = ident[0] == 'f';
		z3::expr_vector xs = ident[0] == 'i'   ? int_args(arg(0))
		                   : ident[0] == 'b' ? bool_args(arg(0))
		                                     : float_args(arg(0));
		return element(xs, int_par(arg(1)), int_arg(arg(2)),
		               ident[0] == 'i'   ? int_arg(arg(3))
		               : ident[0] == 'b' ? bool_arg(arg(3))
		                                 : float_arg(arg(3)),
		               floating);
	}
	if (ident == "int_array_element_nd" || ident == "bool_array_element_nd" ||
	    ident == "float_array_element_nd") {
		want(5);
		bool floating = ident[0] == 'f';
		z3::expr_vector xs = ident[0] == 'i'   ? int_args(arg(0))
		                   : ident[0] == 'b' ? bool_args(arg(0))
		                                     : float_args(arg(0));
		fznso::Value offsets = arg(1);
		fznso::Value sizes = arg(2);
		z3::expr_vector indices = int_args(arg(3));
		if (offsets.size() != indices.size() || sizes.size() != indices.size()) {
			fail(ident, "offsets, sizes and indices differ in length");
		}
		// The array is stored row-major and the dimensions travel beside it,
		// because a list is flat and carries no shape of its own. Folding the
		// indices into one flat index is index arithmetic, not a decomposition.
		z3::expr flat = ctx_.int_val(0);
		z3::expr_vector bounds(ctx_);
		for (unsigned d = 0; d < indices.size(); d++) {
			std::int64_t offset = int_par(offsets[d]);
			std::int64_t extent = int_par(sizes[d]);
			bounds.push_back(ctx_.int_val(offset) <= indices[d] &&
			                 indices[d] <= ctx_.int_val(offset + extent - 1));
			flat = flat * ctx_.int_val(extent) + (indices[d] - ctx_.int_val(offset));
		}
		return z3::mk_and(bounds) && element(xs, 0, flat,
		                                     ident[0] == 'i'   ? int_arg(arg(4))
		                                     : ident[0] == 'b' ? bool_arg(arg(4))
		                                                       : float_arg(arg(4)),
		                                     floating);
	}
	if (ident == "int_if_then_else" || ident == "bool_if_then_else" ||
	    ident == "float_if_then_else") {
		want(3);
		bool floating = ident[0] == 'f';
		z3::expr_vector conditions = bool_args(arg(0));
		z3::expr_vector values = ident[0] == 'i'   ? int_args(arg(1))
		                       : ident[0] == 'b' ? bool_args(arg(1))
		                                         : float_args(arg(1));
		z3::expr result = ident[0] == 'i'   ? int_arg(arg(2))
		                : ident[0] == 'b' ? bool_arg(arg(2))
		                                  : float_arg(arg(2));
		if (conditions.size() != values.size()) {
			fail(ident, "conditions and values differ in length");
		}
		// Stated as the definition states it — one implication per branch —
		// rather than as an `ite` chain, because with no condition holding the
		// result is *unconstrained*, and a chain would force the last value.
		z3::expr_vector cases(ctx_);
		z3::expr_vector earlier(ctx_);
		for (unsigned i = 0; i < conditions.size(); i++) {
			z3::expr first = conditions[i] && z3::mk_and(earlier);
			cases.push_back(z3::implies(
				first, floating ? float_eq(result, values[i]) : result == values[i]));
			earlier.push_back(!conditions[i]);
		}
		return z3::mk_and(cases);
	}

	// --- extrema and membership ---------------------------------------------
	if (ident == "int_array_maximum" || ident == "int_array_minimum") {
		want(2);
		return extremum(int_args(arg(0)), int_arg(arg(1)), ident.find("maximum") != ident.npos,
		                false);
	}
	if (ident == "float_array_maximum" || ident == "float_array_minimum") {
		want(2);
		return extremum(float_args(arg(0)), float_arg(arg(1)),
		                ident.find("maximum") != ident.npos, true);
	}
	if (ident == "int_array_member" || ident == "bool_array_member" ||
	    ident == "float_array_member") {
		want(2);
		z3::expr_vector hits(ctx_);
		if (ident[0] == 'i') {
			z3::expr_vector xs = int_args(arg(0));
			z3::expr value = int_arg(arg(1));
			for (unsigned i = 0; i < xs.size(); i++) {
				hits.push_back(xs[i] == value);
			}
		} else if (ident[0] == 'b') {
			z3::expr_vector xs = bool_args(arg(0));
			z3::expr value = bool_arg(arg(1));
			for (unsigned i = 0; i < xs.size(); i++) {
				hits.push_back(xs[i] == value);
			}
		} else {
			z3::expr_vector xs = float_args(arg(0));
			z3::expr value = float_arg(arg(1));
			for (unsigned i = 0; i < xs.size(); i++) {
				hits.push_back(float_eq(xs[i], value));
			}
		}
		return z3::mk_or(hits);
	}

	// --- counting -----------------------------------------------------------
	if (ident == "int_all_different") {
		want(1);
		z3::expr_vector xs = int_args(arg(0));
		return xs.size() < 2 ? ctx_.bool_val(true) : z3::distinct(xs);
	}
	if (ident == "int_all_equal") {
		want(1);
		z3::expr_vector xs = int_args(arg(0));
		z3::expr_vector same(ctx_);
		for (unsigned i = 1; i < xs.size(); i++) {
			same.push_back(xs[i] == xs[0]);
		}
		return z3::mk_and(same);
	}
	if (ident == "int_count" || ident == "bool_count" || ident == "float_count") {
		want(3);
		if (ident[0] == 'i') {
			z3::expr_vector xs = int_args(arg(0));
			z3::expr value = int_arg(arg(1));
			return int_arg(arg(2)) == count_of(xs, [&](unsigned i) { return xs[i] == value; });
		}
		if (ident[0] == 'b') {
			z3::expr_vector xs = bool_args(arg(0));
			z3::expr value = bool_arg(arg(1));
			return int_arg(arg(2)) == count_of(xs, [&](unsigned i) { return xs[i] == value; });
		}
		z3::expr_vector xs = float_args(arg(0));
		z3::expr value = float_arg(arg(1));
		return int_arg(arg(2)) ==
		       count_of(xs, [&](unsigned i) { return float_eq(xs[i], value); });
	}
	if (ident == "int_among") {
		want(3);
		z3::expr_vector xs = int_args(arg(0));
		fznso::Value values = arg(1);
		return int_arg(arg(2)) ==
		       count_of(xs, [&](unsigned i) { return in_set(xs[i], values); });
	}

	// --- ordering -----------------------------------------------------------
	if (ident == "int_increasing" || ident == "int_strictly_increasing" ||
	    ident == "bool_increasing" || ident == "float_increasing" ||
	    ident == "float_strictly_increasing") {
		want(1);
		bool strict = ident.find("strictly") != ident.npos;
		z3::expr_vector xs = ident[0] == 'i'   ? int_args(arg(0))
		                   : ident[0] == 'b' ? bool_args(arg(0))
		                                     : float_args(arg(0));
		z3::expr_vector steps(ctx_);
		for (unsigned i = 1; i < xs.size(); i++) {
			if (xs[i].is_bool()) {
				// `false` is below `true`, so non-decreasing is implication.
				steps.push_back(z3::implies(xs[i - 1], xs[i]));
			} else {
				steps.push_back(strict ? xs[i - 1] < xs[i] : xs[i - 1] <= xs[i]);
			}
		}
		return z3::mk_and(steps);
	}
	if (ident == "int_value_precede") {
		want(3);
		std::int64_t s = int_par(arg(0));
		std::int64_t t = int_par(arg(1));
		z3::expr_vector xs = int_args(arg(2));
		z3::expr_vector rules(ctx_);
		for (unsigned j = 0; j < xs.size(); j++) {
			z3::expr_vector before(ctx_);
			for (unsigned i = 0; i < j; i++) {
				before.push_back(xs[i] == ctx_.int_val(s));
			}
			rules.push_back(z3::implies(xs[j] == ctx_.int_val(t), z3::mk_or(before)));
		}
		return z3::mk_and(rules);
	}
	if (ident == "int_seq_precede_chain") {
		want(1);
		z3::expr_vector xs = int_args(arg(0));
		// The running maximum may climb by at most one at a time: a value is
		// only available once the one below it has been used.
		//
		// Stated this way rather than as "`xs[j] > 0` needs an `xs[j] - 1`
		// earlier", because the two are not the same constraint and the
		// difference decides whether `[1, 2, 3]` holds. Zero is the *implicit*
		// floor — the maximum over an empty prefix — so a leading 1 is allowed
		// with no 0 before it, which is what `fzn_int_seq_precede_chain.mzn`
		// encodes and what MiniZinc's `seq_precede_chain` documents ("i
		// precedes i+1 for all positive i"). The registry's own formula reads
		// `xs_j > 0` and so refuses `[1, 2, 3]`; the decomposition every other
		// solver gets accepts it, and agreeing with the other solvers is what
		// matters to a model.
		z3::expr_vector rules(ctx_);
		z3::expr running = ctx_.int_val(0);
		for (unsigned j = 0; j < xs.size(); j++) {
			rules.push_back(xs[j] <= running + 1);
			running = z3::ite(xs[j] > running, xs[j], running);
		}
		return z3::mk_and(rules);
	}
	if (ident == "int_lex_lesseq" || ident == "int_lex_less" || ident == "bool_lex_lesseq" ||
	    ident == "bool_lex_less" || ident == "float_lex_lesseq" || ident == "float_lex_less") {
		want(2);
		bool strict = ident.find("_lex_less") != ident.npos &&
		              ident.find("_lex_lesseq") == ident.npos;
		bool floating = ident[0] == 'f';
		if (ident[0] == 'i') {
			return lex(int_args(arg(0)), int_args(arg(1)), strict, false);
		}
		if (ident[0] == 'b') {
			return lex(bool_args(arg(0)), bool_args(arg(1)), strict, false);
		}
		return lex(float_args(arg(0)), float_args(arg(1)), strict, floating);
	}
	if (ident == "int_in") {
		want(2);
		return in_set(int_arg(arg(0)), arg(1));
	}

	// --- extensional --------------------------------------------------------
	if (ident == "int_table" || ident == "bool_table") {
		want(2);
		bool booleans = ident[0] == 'b';
		z3::expr_vector xs = booleans ? bool_args(arg(0)) : int_args(arg(0));
		fznso::Value tuples = arg(1);
		if (xs.empty()) {
			return ctx_.bool_val(true);
		}
		if (tuples.size() % xs.size() != 0) {
			fail(ident, "the table is not a whole number of rows");
		}
		// A disjunction of row conjunctions: the direct statement of "the
		// assignment appears as a row", and the encoding an SMT solver wants.
		std::size_t width = xs.size();
		z3::expr_vector rows(ctx_);
		for (std::size_t r = 0; r * width < tuples.size(); r++) {
			z3::expr_vector cells(ctx_);
			for (std::size_t j = 0; j < width; j++) {
				fznso::Value entry = tuples[r * width + j];
				cells.push_back(xs[static_cast<unsigned>(j)] ==
				                (booleans ? bool_arg(entry) : int_arg(entry)));
			}
			rows.push_back(z3::mk_and(cells));
		}
		return z3::mk_or(rows);
	}

	fail(ident, "this solver does not implement the constraint");
}

// ---------------------------------------------------------------------------
// Objectives and solutions
// ---------------------------------------------------------------------------

z3::expr Translate::objective_term(fznso::Value v, z3::expr_vector& guards) const {
	if (v.kind() != FznsoValueDecision) {
		// A fixed objective: the same problem with a constant cost.
		switch (v.kind()) {
		case FznsoValueInt:
			return ctx_.int_val(static_cast<std::int64_t>(v.as_int()));
		case FznsoValueFloat:
			return ctx_.real_val(static_cast<int>(v.as_float()));
		default:
			throw std::runtime_error("the objective is not a decision variable");
		}
	}
	std::size_t i = v.as_decision().index;
	if (i >= vars_.size()) {
		throw std::runtime_error("the objective names decision " + std::to_string(i) +
		                         ", which the model does not have");
	}
	const z3::expr& x = vars_[i];
	switch (kinds_[i]) {
	case Kind::Int:
		return x;
	case Kind::Bool:
		return z3::ite(x, ctx_.int_val(1), ctx_.int_val(0));
	case Kind::Float:
		break;
	}
	if (encoding_ != FloatEncoding::Fpa) {
		return x;
	}
	// Z3 optimises over integers, reals and bit-vectors only — a FloatingPoint
	// objective is rejected outright — so the objective has to be carried by
	// something else that orders the same way.
	//
	// The obvious route, `fp.to_real`, is unusable: it puts a bit-blasted
	// FloatingPoint term inside the linear-arithmetic theory, and maximising
	// one over a variable already pinned to `0.0 .. 4.5` does not terminate.
	//
	// IEEE-754 was designed so that its values sort in the order of their bit
	// patterns read as sign-magnitude, so the order-preserving map into an
	// unsigned 64-bit key is exact and stays entirely inside the bit-vector
	// theory the FloatingPoint terms are already blasted into. Same answer,
	// and it finishes in a fraction of a second.
	//
	//     key(f) = to_ieee_bv(f) with the sign bit flipped when it is clear,
	//              and every bit flipped when it is set
	//
	// The infinities need no special case — they are the extremes of that
	// order, which is exactly right. NaN is not in the order at all, so it is
	// ruled out; that is the one constraint this adds, and the caller says so.
	guards.push_back(!own(Z3_mk_fpa_is_nan(ctx_, x)));
	z3::expr bits = own(Z3_mk_fpa_to_ieee_bv(ctx_, x));
	z3::expr sign = own(Z3_mk_extract(ctx_, 63, 63, bits));
	z3::expr rest = own(Z3_mk_extract(ctx_, 62, 0, bits));
	z3::expr high = ctx_.bv_val(1, 1);
	z3::expr positive = own(Z3_mk_concat(ctx_, high, rest));
	return z3::ite(sign == ctx_.bv_val(0, 1), positive, ~bits);
}

fznso::OwnedValue Translate::read(const z3::model& m, std::size_t d) const {
	if (d >= vars_.size()) {
		return fznso::OwnedValue{};
	}
	// Completion on: a decision the constraints do not pin down still needs a
	// value if the model asked for one.
	switch (kinds_[d]) {
	case Kind::Bool: {
		z3::expr v = m.eval(vars_[d], true);
		return fznso::OwnedValue{v.is_true()};
	}
	case Kind::Int: {
		z3::expr v = m.eval(vars_[d], true);
		std::int64_t out = 0;
		if (!v.is_numeral_i64(out)) {
			// Z3's integers are unbounded and this interface's are not. Saying
			// so is the only honest answer: a wrapped value would be a wrong
			// answer that nothing downstream could detect.
			throw std::runtime_error("decision " + std::to_string(d) + " is assigned " +
			                         v.to_string() +
			                         ", which does not fit a 64-bit integer");
		}
		return fznso::OwnedValue{out};
	}
	case Kind::Float:
		break;
	}
	if (encoding_ == FloatEncoding::Fpa) {
		z3::expr v = m.eval(vars_[d], true);
		if (Z3_fpa_is_numeral_nan(ctx_, v)) {
			return fznso::OwnedValue{std::nan("")};
		}
		if (Z3_fpa_is_numeral_inf(ctx_, v)) {
			double inf = std::numeric_limits<double>::infinity();
			return fznso::OwnedValue{Z3_fpa_is_numeral_negative(ctx_, v) ? -inf : inf};
		}
		// Everything finite crosses into the reals exactly, and is read back
		// from there — there is no direct FloatingPoint-to-`double` accessor.
		z3::expr as_real = m.eval(to_real(vars_[d]), true).simplify();
		return fznso::OwnedValue{as_real.as_double()};
	}
	z3::expr v = m.eval(vars_[d], true).simplify();
	if (v.is_numeral()) {
		return fznso::OwnedValue{v.as_double()};
	}
	// Nonlinear real arithmetic can settle on an algebraic irrational, which
	// has no exact `double`. A 17-digit decimal is the closest this interface
	// can carry.
	return fznso::OwnedValue{std::stod(v.get_decimal_string(17))};
}

} // namespace fznso_z3
