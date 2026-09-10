#include "declarations.hh"

namespace fznso_z3 {

namespace {

/// Storage the option defaults borrow. A `fznso::Value` points at its payload
/// rather than copying it, so these outlive every use of the list.
struct Defaults {
	bool no = false;
	std::int64_t one = 1;
	std::string fpa = "float";
};

const Defaults& defaults() {
	static const Defaults d;
	return d;
}

} // namespace

void Declarations::add(std::string ident, std::vector<FznsoType> args) {
	names_.push_back(std::move(ident));
	arguments_.push_back(std::move(args));
}

void Declarations::add_reified(const std::string& ident, std::vector<FznsoType> args,
                               bool with_imp) {
	std::vector<FznsoType> reified = args;
	reified.push_back(kVB);
	add(ident, std::move(args));
	add(ident + "_reif", reified);
	if (with_imp) {
		add(ident + "_imp", std::move(reified));
	}
}

const Declarations& Declarations::get() {
	static const Declarations d;
	return d;
}

Declarations::Declarations() {
	// Everything declared here is one quantifier-free formula over the
	// arguments: no auxiliary decision variable, no second constraint posted.
	// That is what "native" means for an SMT solver — its propagator *is* the
	// formula — and it is the line this list is drawn on. A constraint that
	// would need an introduced variable is left to the library, however easy it
	// would be to write, because a declaration MiniZinc believes and the solver
	// cannot honour is worse than no declaration at all.

	// --- integer arithmetic -------------------------------------------------
	add_reified("int_lin_eq", {kAI, kAVI, kI});
	add_reified("int_lin_le", {kAI, kAVI, kI});
	add_reified("int_lin_ne", {kAI, kAVI, kI});
	// Functional: one argument is a variable the others determine, so there is
	// nothing to reify. The registry says so, and `fznso-conform` checks it.
	add("int_abs", {kVI, kVI});
	add("int_times", {kVI, kVI, kVI});
	add("int_div", {kVI, kVI, kVI});
	add("int_mod", {kVI, kVI, kVI});
	// Narrowed to a parameter exponent: a variable exponent is not something Z3
	// decides over the integers. `redefinitions-2.2.1.mzn` routes `int_pow_fixed`
	// onto this name, so the narrowed form is what MiniZinc actually emits, and
	// `add_fixed_form` generates the dispatch for the rest.
	add("int_pow", {kVI, kI, kVI});
	add("int_to_float", {kVI, kVF});

	// --- Booleans -----------------------------------------------------------
	add_reified("bool_clause", {kAVB, kAVB});
	add("bool_array_and", {kAVB, kVB});
	add_reified("bool_array_xor", {kAVB});
	add("bool_lin_eq", {kAI, kAVB, kVI});
	add_reified("bool_lin_le", {kAI, kAVB, kI});
	add_reified("bool_lin_ne", {kAI, kAVB, kVI});
	add("bool_to_int", {kVB, kVI});

	// --- floats -------------------------------------------------------------
	//
	// The sort depends on `z3_float_encoding`, but the *declared list* does not:
	// every name here is postable under both encodings, which is what makes the
	// encoding an option rather than a second library. `float_sqrt` is the one
	// that differs in kind — `fp.sqrt` under FPA, `b*b = a /\ b >= 0` under
	// reals — and both are one formula over the arguments.
	add_reified("float_lin_eq", {kAF, kAVF, kF});
	add_reified("float_lin_le", {kAF, kAVF, kF});
	add_reified("float_lin_lt", {kAF, kAVF, kF});
	add_reified("float_lin_ne", {kAF, kAVF, kF});
	add("float_abs", {kVF, kVF});
	add("float_times", {kVF, kVF, kVF});
	add("float_div", {kVF, kVF, kVF});
	add("float_sqrt", {kVF, kVF});
	add("float_ceil", {kVF, kVI});
	add("float_floor", {kVF, kVI});
	add("float_round", {kVF, kVI});
	add_reified("float_in", {kVF, kF, kF});
	// `float_pow` is absent too, and for a sharper reason than the rest: the
	// registry types its exponent `var float`, and an `int` is not a narrowing
	// of that but a different type. A genuine float power exists only over the
	// reals (`Z3_mk_power`, through nlsat) and not at all over FloatingPoint,
	// so one declaration cannot serve both encodings honestly.
	//
	// The transcendentals are deliberately absent: Z3 has no theory for them and
	// there is no decomposition anywhere, so a model using one fails loudly,
	// exactly as it does for a MIP solver.

	// --- arrays -------------------------------------------------------------
	//
	// An element constraint is a nested `ite` chain over the index, which is a
	// term rather than a set of clauses. The offset is index arithmetic.
	add("int_array_element", {kAVI, kI, kVI, kVI});
	add("bool_array_element", {kAVB, kI, kVI, kVB});
	add("float_array_element", {kAVF, kI, kVI, kVF});
	add("int_array_element_nd", {kAVI, kAI, kAI, kAVI, kVI});
	add("bool_array_element_nd", {kAVB, kAI, kAI, kAVI, kVB});
	add("float_array_element_nd", {kAVF, kAI, kAI, kAVI, kVF});
	add("int_if_then_else", {kAVB, kAVI, kVI});
	add("bool_if_then_else", {kAVB, kAVB, kVB});
	add("float_if_then_else", {kAVB, kAVF, kVF});
	add("int_array_maximum", {kAVI, kVI});
	add("int_array_minimum", {kAVI, kVI});
	add("float_array_maximum", {kAVF, kVF});
	add("float_array_minimum", {kAVF, kVF});
	add_reified("int_array_member", {kAVI, kVI});
	// No `_imp` for this one. Its last argument is a scalar `var bool`, which is
	// the shape MiniZinc uses to recognise a *reified* predicate
	// (`native_predicates.cpp:280`) — `array_bool_or` is one, and has no `_reif`
	// suffix to go by. So `fzn_bool_array_member(xs, value)` is read as already
	// reified on `value`, and declaring `bool_array_member_imp` is exactly what
	// lets the flattener emit a two-argument `fzn_bool_array_member_imp` to
	// match it. That collides with the three-argument form declared here, and
	// every model reaching it fails to typecheck. The reified form is fine; only
	// the derived half-reified one is not.
	add_reified("bool_array_member", {kAVB, kVB}, /*with_imp=*/false);
	add_reified("float_array_member", {kAVF, kVF});

	// --- ordering and counting ----------------------------------------------
	add_reified("int_all_different", {kAVI});
	add_reified("int_all_equal", {kAVI});
	add("int_count", {kAVI, kVI, kVI});
	add("bool_count", {kAVB, kVB, kVI});
	add("float_count", {kAVF, kVF, kVI});
	add("int_among", {kAVI, kS, kVI});
	add_reified("int_increasing", {kAVI});
	add_reified("bool_increasing", {kAVB});
	add_reified("float_increasing", {kAVF});
	add_reified("int_strictly_increasing", {kAVI});
	add_reified("float_strictly_increasing", {kAVF});
	add_reified("int_value_precede", {kI, kI, kAVI});
	add_reified("int_seq_precede_chain", {kAVI});
	add_reified("int_lex_lesseq", {kAVI, kAVI});
	add_reified("int_lex_less", {kAVI, kAVI});
	add_reified("bool_lex_lesseq", {kAVB, kAVB});
	add_reified("bool_lex_less", {kAVB, kAVB});
	add_reified("float_lex_lesseq", {kAVF, kAVF});
	add_reified("float_lex_less", {kAVF, kAVF});
	// Narrowed: the registry types the set argument `var set of int`, and this
	// solver has no set sort. `add_fixed_form` generates the dispatch, and the
	// variable-set case falls through to `fznso_nosets/fzn_int_in.mzn`, which is
	// on the include path precisely because no set decision type is declared.
	//
	add_reified("int_in", {kVI, kS});

	// --- extensional --------------------------------------------------------
	//
	// A disjunction of row conjunctions: the canonical SMT encoding, and far
	// smaller than the decomposition the library would otherwise use.
	add_reified("int_table", {kAVI, kAI});
	add_reified("bool_table", {kAVB, kAB});

	// `int_span` is deliberately absent, though it has no decomposition either.
	// Its definition takes the minimum and maximum "over the present sub-tasks
	// only" — `start_i != _|_` — but the arguments it is declared with carry no
	// `opt` flag, so there is no way to tell a present task from an absent one
	// through the interface. Nothing in MiniZinc lowers to it yet, so guessing
	// costs more than it could possibly buy.

	constraints_.reserve(names_.size());
	for (std::size_t i = 0; i < names_.size(); i++) {
		const std::vector<FznsoType>& a = arguments_[i];
		constraints_.push_back(FznsoConstraintType{fznso::str(names_[i]), a.size(), a.data()});
	}

	// No `var set of int`: Z3 has no set-of-int theory, and saying so is what
	// puts `fznso_nosets/` on the flattener's include path.
	decisions_ = {kVB, kVI, kVF};

	// All ten registry strategies. Z3's optimizer takes several objectives at
	// once and its `priority` parameter chooses how they combine, so the lex and
	// Pareto forms are the same machinery as the single ones rather than a
	// separate implementation.
	objectives_ = {
		FznsoObjective{fznso::str("int_minimize"), kVI},
		FznsoObjective{fznso::str("int_maximize"), kVI},
		FznsoObjective{fznso::str("float_minimize"), kVF},
		FznsoObjective{fznso::str("float_maximize"), kVF},
		FznsoObjective{fznso::str("int_lex_minimize"), kAVI},
		FznsoObjective{fznso::str("int_lex_maximize"), kAVI},
		FznsoObjective{fznso::str("float_lex_minimize"), kAVF},
		FznsoObjective{fznso::str("float_lex_maximize"), kAVF},
		FznsoObjective{fznso::str("int_pareto_maximize"), kAVI},
		FznsoObjective{fznso::str("float_pareto_maximize"), kAVF},
	};

	options_ = {
		FznsoOption{fznso::str("all_solutions"), kB, fznso::Value{defaults().no}.raw()},
		FznsoOption{fznso::str("intermediate"), kB, fznso::Value{defaults().no}.raw()},
		// `threads` is deliberately absent. Z3's optimiser does not search on
		// several threads, and the registry entry is a claim that it could —
		// declaring it would mean accepting a number and ignoring it.
		FznsoOption{fznso::str("time_limit"), kOptI, fznso::Value{}.raw()},
		FznsoOption{fznso::str("random_seed"), kOptI, fznso::Value{}.raw()},
		FznsoOption{fznso::str("verbose"), kB, fznso::Value{defaults().no}.raw()},
		// Not a registry name, so it carries the solver's own prefix. See
		// `translate.hh` for what the two encodings mean.
		FznsoOption{fznso::str("z3_float_encoding"), kStr,
		            fznso::Value{defaults().fpa}.raw()},
	};

	// `{ident, type, from a solution, from the solver}`.
	statistics_ = {
		FznsoStatistic{fznso::str("solutions"), kI, true, true},
		FznsoStatistic{fznso::str("int_objective"), kI, true, true},
		FznsoStatistic{fznso::str("float_objective"), kF, true, true},
		FznsoStatistic{fznso::str("int_objective_bound"), kI, false, true},
		FznsoStatistic{fznso::str("float_objective_bound"), kF, false, true},
		FznsoStatistic{fznso::str("decisions"), kI, false, true},
		FznsoStatistic{fznso::str("constraints"), kI, false, true},
		FznsoStatistic{fznso::str("init_time"), kF, false, true},
		FznsoStatistic{fznso::str("solve_time"), kF, false, true},
		// Z3's own counters, and one of our own: how many layers the last run
		// had to post. Zero means the whole model was reused, which is the only
		// direct evidence that incrementality is working — see the self-test.
		FznsoStatistic{fznso::str("z3_layers_posted"), kI, false, true},
		FznsoStatistic{fznso::str("z3_conflicts"), kI, false, true},
		FznsoStatistic{fznso::str("z3_decisions"), kI, false, true},
		FznsoStatistic{fznso::str("z3_propagations"), kI, false, true},
		FznsoStatistic{fznso::str("z3_memory"), kF, false, true},
	};
}

} // namespace fznso_z3
