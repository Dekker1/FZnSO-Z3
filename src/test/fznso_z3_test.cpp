// End-to-end checks for the Z3 backend, through the FZnSO protocol.
//
// The solver is loaded the way a real consumer loads it — through
// `fznso::Library`, from the built shared object — so the entry-point naming and
// the export path are exercised along with the translation itself.
//
// This file is the *only* gate that reaches layers, multi-objective search and
// unbounded integers: no MiniZinc-level test can produce them, because MiniZinc
// creates a fresh solver per run and never asks for a lexicographic objective.
//
// Usage: fznso-z3-test <path to libz3.dylib|.so|.dll> [minizinc library dir]

#include "fznso.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

using fznso::Decision;
using fznso::LayeredModel;
using fznso::OwnedValue;
using fznso::Solution;
using fznso::Status;

constexpr FznsoType BOOL = fznso::Type{FznsoTypeBaseBool}.decision(true);
constexpr FznsoType INT = fznso::Type{FznsoTypeBaseInt}.decision(true);
constexpr FznsoType FLOAT = fznso::Type{FznsoTypeBaseFloat}.decision(true);

int failures = 0;

void check(bool ok, const char* what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		failures++;
	}
}

OwnedValue num(std::int64_t v) { return OwnedValue{v}; }
OwnedValue list(std::vector<OwnedValue> items) { return OwnedValue{std::move(items)}; }

/// One run's answers, collected so the assertions can look at them afterwards.
struct Collected {
	std::vector<std::vector<OwnedValue>> solutions;
	std::vector<std::pair<std::string, std::string>> messages;
	Status status{Status::Kind::Complete, {}};
};

/// Read `count` decisions out of every solution of `model`.
Collected solve(fznso::DynSolver& solver, const LayeredModel& model, std::size_t count,
                std::size_t stop_after = 0) {
	Collected out;
	bool stop = false;
	out.status = solver.run(
		model,
		[&](const Solution& s) {
			std::vector<OwnedValue> values;
			values.reserve(count);
			for (std::size_t i = 0; i < count; i++) {
				values.emplace_back(s[Decision{i}]);
			}
			out.solutions.push_back(std::move(values));
			if (stop_after != 0 && out.solutions.size() >= stop_after) {
				stop = true;
			}
		},
		[&](std::string_view scope, fznso::Value value) {
			out.messages.emplace_back(std::string(scope), value.kind() == FznsoValueString
			                                                  ? std::string(value.as_string())
			                                                  : std::string());
		},
		[&]() { return stop; });
	return out;
}

Collected solve(fznso::Library& lib, const LayeredModel& model, std::size_t count,
                const std::vector<std::pair<std::string, OwnedValue>>& options = {},
                std::size_t stop_after = 0) {
	fznso::DynSolver solver = lib.create_solver();
	for (const auto& o : options) {
		std::optional<std::string> error = solver.option_set(o.first, fznso::Value{o.second});
		check(!error.has_value(), error.has_value() ? error->c_str() : "option rejected");
	}
	return solve(solver, model, count, stop_after);
}

std::int64_t as_int(const OwnedValue& v) { return fznso::Value{v}.as_int(); }
bool as_bool(const OwnedValue& v) { return fznso::Value{v}.as_bool(); }
double as_float(const OwnedValue& v) { return fznso::Value{v}.as_float(); }
bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

/// Post `Σ coeffs·xs <rel> rhs`. The registry has no `int_le`: every integer
/// inequality is `int_lin_le` with a coefficient list.
void lin(LayeredModel& m, const char* ident, std::vector<std::int64_t> coeffs,
         std::vector<OwnedValue> xs, std::int64_t rhs) {
	std::vector<OwnedValue> cs;
	cs.reserve(coeffs.size());
	for (std::int64_t c : coeffs) {
		cs.push_back(num(c));
	}
	m.add_constraint(ident, {list(std::move(cs)), list(std::move(xs)), num(rhs)});
}

// --- the checks ------------------------------------------------------------

/// `x + y = 10`, `x ≤ y`, over `0..10`, with `x` maximised: the unique answer is
/// `x = y = 5`.
void int_model(fznso::Library& lib) {
	LayeredModel m;
	Decision x = m.add_decision(INT, OwnedValue::int_range(0, 10), "x");
	Decision y = m.add_decision(INT, OwnedValue::int_range(0, 10), "y");
	lin(m, "int_lin_eq", {1, 1}, {OwnedValue{x}, OwnedValue{y}}, 10);
	lin(m, "int_lin_le", {1, -1}, {OwnedValue{x}, OwnedValue{y}}, 0);
	m.set_objective("int_maximize", OwnedValue{x});

	Collected out = solve(lib, m, 2);
	check(out.status.complete(), "int model completes");
	check(out.solutions.size() == 1, "int model reports one solution");
	if (!out.solutions.empty()) {
		check(as_int(out.solutions[0][0]) == 5 && as_int(out.solutions[0][1]) == 5,
		      "int model maximises x to 5");
	}
}

/// A `var int` with **no domain at all**. Z3's integers are unbounded, so this
/// is the ordinary case here — and the case neither existing FZnSO solver can
/// express, since both need a finite domain to build a variable from.
void unbounded_int(fznso::Library& lib) {
	LayeredModel m;
	Decision x = m.add_decision(INT, OwnedValue{}, "x");
	// `x * x = 49`, `x > 0`: the answer is 7, and nothing bounds the search.
	m.add_constraint("int_times", {OwnedValue{x}, OwnedValue{x}, num(49)});
	lin(m, "int_lin_le", {-1}, {OwnedValue{x}}, -1);

	Collected out = solve(lib, m, 1);
	check(out.status.complete(), "unbounded model completes");
	check(out.solutions.size() == 1, "unbounded model reports one solution");
	if (!out.solutions.empty()) {
		check(as_int(out.solutions[0][0]) == 7, "unbounded model finds x = 7");
	}
}

/// An objective with nothing bounding it. Z3 answers `sat` with the objective
/// `oo`, which is not a solution to report — so it is an error, not an answer.
void unbounded_objective(fznso::Library& lib) {
	LayeredModel m;
	Decision x = m.add_decision(INT, OwnedValue{}, "x");
	lin(m, "int_lin_le", {-1}, {OwnedValue{x}}, -4);
	m.set_objective("int_maximize", OwnedValue{x});

	Collected out = solve(lib, m, 1);
	check(out.status.failed(), "an unbounded objective fails");
	check(out.solutions.empty(), "an unbounded objective reports no solution");
}

/// MiniZinc's `int_div` truncates toward zero; SMT-LIB's `div` floors. Posting
/// Z3's operator unchanged would be a silent wrong answer on every negative
/// dividend, so all four sign combinations are pinned here.
void integer_division(fznso::Library& lib) {
	struct Case {
		std::int64_t a;
		std::int64_t b;
		std::int64_t q;
		std::int64_t r;
	};
	// Truncating division, and a remainder with the sign of the dividend.
	const Case cases[] = {
		{7, 2, 3, 1}, {-7, 2, -3, -1}, {7, -2, -3, 1}, {-7, -2, 3, -1}, {6, 3, 2, 0}, {-6, 3, -2, 0},
	};
	for (const Case& c : cases) {
		LayeredModel m;
		Decision q = m.add_decision(INT, OwnedValue{}, "q");
		Decision r = m.add_decision(INT, OwnedValue{}, "r");
		m.add_constraint("int_div", {num(c.a), num(c.b), OwnedValue{q}});
		m.add_constraint("int_mod", {num(c.a), num(c.b), OwnedValue{r}});
		Collected out = solve(lib, m, 2);
		std::string what = std::to_string(c.a) + " div/mod " + std::to_string(c.b);
		check(out.solutions.size() == 1, ("division: " + what + " has an answer").c_str());
		if (out.solutions.empty()) {
			continue;
		}
		check(as_int(out.solutions[0][0]) == c.q, ("division: " + what + " quotient").c_str());
		check(as_int(out.solutions[0][1]) == c.r, ("division: " + what + " remainder").c_str());
	}

	// Division by zero must fail rather than pick a value: Z3's `div` is total
	// and uninterpreted there, FlatZinc's is not.
	LayeredModel m;
	Decision q = m.add_decision(INT, OwnedValue::int_range(-100, 100), "q");
	m.add_constraint("int_div", {num(7), num(0), OwnedValue{q}});
	Collected out = solve(lib, m, 1);
	check(out.status.complete() && out.solutions.empty(), "division by zero is unsatisfiable");
}

/// Reification and half-reification, which for an SMT solver are the base
/// formula under an equivalence and under an implication.
void reification(fznso::Library& lib) {
	// `b <-> x >= 5`, with `x = 3`: `b` must be false.
	{
		LayeredModel m;
		Decision b = m.add_decision(BOOL, OwnedValue{}, "b");
		m.add_constraint("int_lin_le_reif",
		                 {list({num(-1)}), list({num(3)}), num(-5), OwnedValue{b}});
		Collected out = solve(lib, m, 1);
		check(out.solutions.size() == 1 && !as_bool(out.solutions[0][0]),
		      "int_lin_le_reif is false when the constraint does not hold");
	}
	// `b -> x >= 5` with `b` true forces `x >= 5`; minimising `x` gives 5.
	{
		LayeredModel m;
		Decision x = m.add_decision(INT, OwnedValue::int_range(0, 9), "x");
		m.add_constraint("int_lin_le_imp", {list({num(-1)}), list({OwnedValue{x}}), num(-5),
		                                    OwnedValue{true}});
		m.set_objective("int_minimize", OwnedValue{x});
		Collected out = solve(lib, m, 1);
		check(out.solutions.size() == 1 && as_int(out.solutions[0][0]) == 5,
		      "int_lin_le_imp propagates from a true antecedent");
	}
	// The half-reified form leaves `b` free when the constraint holds, so
	// minimising `b` gives false even though `x >= 5`.
	{
		LayeredModel m;
		Decision b = m.add_decision(BOOL, OwnedValue{}, "b");
		m.add_constraint("int_lin_le_imp",
		                 {list({num(-1)}), list({num(7)}), num(-5), OwnedValue{b}});
		m.add_constraint("bool_lin_le", {list({num(1)}), list({OwnedValue{b}}), num(0)});
		Collected out = solve(lib, m, 1);
		check(out.solutions.size() == 1 && !as_bool(out.solutions[0][0]),
		      "int_lin_le_imp leaves its Boolean free when the constraint holds");
	}
}

/// The layer mechanism: three runs on **one** solver instance, asserting both
/// the answers and how much was re-posted.
///
/// The `z3_layers_posted` statistic is what makes the second half checkable. An
/// incremental solver that quietly rebuilt everything would give the same
/// answers, so the answers alone prove nothing.
void layers(fznso::Library& lib) {
	fznso::DynSolver solver = lib.create_solver();
	check(!solver.option_set("all_solutions", fznso::Value{true}).has_value(),
	      "all_solutions accepted");

	auto posted = [&]() {
		fznso::Value v = solver.statistic("z3_layers_posted");
		return v.kind() == FznsoValueInt ? v.as_int() : -1;
	};

	LayeredModel m;
	Decision x = m.add_decision(INT, OwnedValue::int_range(0, 10), "x");
	lin(m, "int_lin_le", {-1}, {OwnedValue{x}}, -3); // 3 <= x

	Collected first = solve(solver, m, 1);
	check(first.status.complete() && first.solutions.size() == 8,
	      "layer 0 alone has eight solutions");
	check(posted() == 1, "the first run posts layer 0");

	// A second layer narrows the model. Layer 0 is untouched, so the solver may
	// keep the assertion scope it built for it.
	m.push_layer();
	Decision y = m.add_decision(INT, OwnedValue::int_range(0, 10), "y");
	lin(m, "int_lin_le", {1}, {OwnedValue{x}}, 5); // x <= 5
	lin(m, "int_lin_eq", {1, -1}, {OwnedValue{x}, OwnedValue{y}}, 0);
	m.set_unchanged(1);

	Collected second = solve(solver, m, 2);
	check(second.status.complete() && second.solutions.size() == 3,
	      "with layer 1 only 3..5 remain");
	check(posted() == 1, "the second run posts only the new layer");
	for (const std::vector<OwnedValue>& s : second.solutions) {
		check(as_int(s[0]) >= 3 && as_int(s[0]) <= 5, "layer 0's constraint still holds");
		check(as_int(s[0]) == as_int(s[1]), "layer 1's constraint holds");
	}

	// Retracting the layer restores the original answers, and costs nothing:
	// a popped scope is gone, and there is nothing above it to rebuild.
	m.pop_layer();
	m.set_unchanged(1);
	Collected third = solve(solver, m, 1);
	check(third.status.complete() && third.solutions.size() == 8,
	      "popping layer 1 restores eight solutions");
	check(posted() == 0, "popping a layer posts nothing again");
	for (const std::vector<OwnedValue>& s : third.solutions) {
		check(as_int(s[0]) >= 3, "the retracted layer really is gone");
	}
	(void)y;
}

/// `permanent` is not a promise the solver has seen a layer: only `unchanged`
/// is. An under-reported `unchanged` rebuilds even a permanent layer.
void permanent_layers(fznso::Library& lib) {
	fznso::DynSolver solver = lib.create_solver();
	auto posted = [&]() { return solver.statistic("z3_layers_posted").as_int(); };

	LayeredModel m;
	Decision x = m.add_decision(INT, OwnedValue::int_range(0, 10), "x");
	lin(m, "int_lin_le", {-1}, {OwnedValue{x}}, -3);
	m.mark_permanent();

	Collected first = solve(solver, m, 1);
	check(first.status.complete() && !first.solutions.empty(), "permanent layer 0 solves");
	check(posted() == 1, "the first run posts the permanent layer");

	// The consumer says nothing is unchanged. Layer 0 is permanent, but that does
	// not say the solver saw it as it is now, so it is rebuilt.
	m.set_unchanged(0);
	Collected second = solve(solver, m, 1);
	check(second.status.complete() && !second.solutions.empty(), "permanent layer still solves");
	check(posted() == 1, "an under-reported `unchanged` rebuilds a permanent layer");

	// A redundant permanent layer may be dropped or kept, but either way the
	// answers must not change.
	m.push_layer();
	lin(m, "int_lin_le", {-1}, {OwnedValue{x}}, -5); // 5 <= x, subsuming 3 <= x
	m.mark_permanent();
	m.mark_redundant(0);
	m.set_unchanged(1);
	Collected third = solve(solver, m, 1);
	check(third.status.complete() && !third.solutions.empty(), "a redundant layer still solves");
	if (!third.solutions.empty()) {
		check(as_int(third.solutions[0][0]) >= 5, "the subsuming layer holds");
	}
}

/// Only `unchanged` says the solver has seen a layer as it is now. A consumer can
/// pop a layer, push a different one and mark it permanent before the next run,
/// so `permanent` exceeding `unchanged` must still rebuild the replaced layer.
/// Lexicographic optimisation by pushing and pinning bounds is exactly this.
void replaced_permanent_layer(fznso::Library& lib) {
	fznso::DynSolver solver = lib.create_solver();
	check(!solver.option_set("all_solutions", fznso::Value{true}).has_value(),
	      "all_solutions accepted");

	LayeredModel m;
	Decision x = m.add_decision(INT, OwnedValue::int_range(0, 10), "x");
	solve(solver, m, 1);

	m.push_layer();
	lin(m, "int_lin_le", {1}, {OwnedValue{x}}, 3); // x <= 3
	m.set_unchanged(1);
	solve(solver, m, 1);

	m.pop_layer();
	m.push_layer();
	lin(m, "int_lin_le", {-1}, {OwnedValue{x}}, -7); // x >= 7
	m.mark_permanent();
	m.set_unchanged(1);
	Collected out = solve(solver, m, 1);
	check(out.status.complete(), "replaced permanent layer completes");
	std::vector<std::int64_t> xs;
	for (const std::vector<OwnedValue>& s : out.solutions) {
		xs.push_back(as_int(s[0]));
	}
	std::sort(xs.begin(), xs.end());
	check(xs == std::vector<std::int64_t>{7, 8, 9, 10},
	      "a layer replaced and marked permanent is posted, not the popped one");
}

/// Lexicographic minimisation, on a model where it and the sum disagree.
void lex_objective(fznso::Library& lib) {
	LayeredModel m;
	Decision a = m.add_decision(INT, OwnedValue::int_range(0, 9), "a");
	Decision b = m.add_decision(INT, OwnedValue::int_range(0, 9), "b");
	// `a + b >= 9`. Minimising the sum is every point on the line; minimising
	// lexicographically pins `a = 0`, then `b = 9`.
	lin(m, "int_lin_le", {-1, -1}, {OwnedValue{a}, OwnedValue{b}}, -9);
	m.set_objective("int_lex_minimize", list({OwnedValue{a}, OwnedValue{b}}));

	Collected out = solve(lib, m, 2);
	check(out.status.complete(), "lexicographic minimisation completes");
	check(out.solutions.size() == 1, "lexicographic minimisation reports one solution");
	if (!out.solutions.empty()) {
		check(as_int(out.solutions[0][0]) == 0 && as_int(out.solutions[0][1]) == 9,
		      "lexicographic minimisation prefers a = 0 over a smaller sum");
	}
}

/// Pareto search reports a *set* of incomparable solutions, one per check,
/// rather than converging on one. The frontier here is exactly three points.
void pareto_objective(fznso::Library& lib) {
	LayeredModel m;
	Decision a = m.add_decision(INT, OwnedValue::int_range(1, 3), "a");
	Decision b = m.add_decision(INT, OwnedValue::int_range(1, 3), "b");
	lin(m, "int_lin_le", {1, 1}, {OwnedValue{a}, OwnedValue{b}}, 4);
	m.set_objective("int_pareto_maximize", list({OwnedValue{a}, OwnedValue{b}}));

	Collected out = solve(lib, m, 2);
	check(out.status.complete(), "Pareto search completes");
	check(out.solutions.size() == 3, "the frontier has three points");
	std::vector<std::pair<std::int64_t, std::int64_t>> got;
	for (const std::vector<OwnedValue>& s : out.solutions) {
		got.emplace_back(as_int(s[0]), as_int(s[1]));
	}
	auto has = [&](std::int64_t p, std::int64_t q) {
		for (const auto& g : got) {
			if (g.first == p && g.second == q) {
				return true;
			}
		}
		return false;
	};
	check(has(1, 3) && has(2, 2) && has(3, 1), "the frontier is exactly (1,3), (2,2), (3,1)");
}

/// Floats, under both encodings. The values are chosen to be exactly
/// representable, so the two must agree: a difference here is a bug in the
/// encoding switch rather than in IEEE-754.
void float_model(fznso::Library& lib) {
	for (const char* encoding : {"float", "real"}) {
		LayeredModel m;
		Decision f = m.add_decision(FLOAT, OwnedValue::float_range(0.0, 10.0), "f");
		// `2.0 * f = 5.0`, so `f = 2.5`.
		m.add_constraint("float_lin_eq",
		                 {list({OwnedValue{2.0}}), list({OwnedValue{f}}), OwnedValue{5.0}});
		Collected out =
			solve(lib, m, 1, {{"z3_float_encoding", OwnedValue{std::string{encoding}}}});
		std::string what = std::string{"float model ("} + encoding + ")";
		check(out.solutions.size() == 1, (what + " has an answer").c_str());
		if (!out.solutions.empty()) {
			check(near(as_float(out.solutions[0][0]), 2.5), (what + " gives f = 2.5").c_str());
		}
	}
}

/// A float objective. Z3 rejects a FloatingPoint objective outright, so under
/// that encoding the objective crosses into the reals — and says so.
void float_objective(fznso::Library& lib) {
	for (const char* encoding : {"float", "real"}) {
		LayeredModel m;
		Decision f = m.add_decision(FLOAT, OwnedValue::float_range(0.0, 10.0), "f");
		m.add_constraint("float_lin_le",
		                 {list({OwnedValue{1.0}}), list({OwnedValue{f}}), OwnedValue{4.5}});
		m.set_objective("float_maximize", OwnedValue{f});
		Collected out =
			solve(lib, m, 1, {{"z3_float_encoding", OwnedValue{std::string{encoding}}}});
		std::string what = std::string{"float objective ("} + encoding + ")";
		check(out.status.complete(), (what + " completes").c_str());
		check(out.solutions.size() == 1, (what + " has an answer").c_str());
		if (!out.solutions.empty()) {
			check(near(as_float(out.solutions[0][0]), 4.5), (what + " maximises to 4.5").c_str());
		}
	}
}

/// The globals that are one formula: `distinct`, a table, an element lookup.
void globals(fznso::Library& lib) {
	{
		LayeredModel m;
		Decision a = m.add_decision(INT, OwnedValue::int_range(1, 3), "a");
		Decision b = m.add_decision(INT, OwnedValue::int_range(1, 3), "b");
		Decision c = m.add_decision(INT, OwnedValue::int_range(1, 3), "c");
		m.add_constraint("int_all_different",
		                 {list({OwnedValue{a}, OwnedValue{b}, OwnedValue{c}})});
		lin(m, "int_lin_eq", {1}, {OwnedValue{a}}, 2);
		lin(m, "int_lin_eq", {1}, {OwnedValue{b}}, 1);
		Collected out = solve(lib, m, 3);
		check(out.solutions.size() == 1 && as_int(out.solutions[0][2]) == 3,
		      "int_all_different forces the remaining value");
	}
	{
		// The table's rows are `(1,2)` and `(3,4)`; pinning the first column to
		// 3 forces the second to 4.
		LayeredModel m;
		Decision a = m.add_decision(INT, OwnedValue::int_range(0, 9), "a");
		Decision b = m.add_decision(INT, OwnedValue::int_range(0, 9), "b");
		m.add_constraint("int_table", {list({OwnedValue{a}, OwnedValue{b}}),
		                               list({num(1), num(2), num(3), num(4)})});
		lin(m, "int_lin_eq", {1}, {OwnedValue{a}}, 3);
		Collected out = solve(lib, m, 2);
		check(out.solutions.size() == 1 && as_int(out.solutions[0][1]) == 4,
		      "int_table picks the matching row");
	}
	{
		// `xs = [10, 20, 30]` numbered from 1; index 2 gives 20.
		LayeredModel m;
		Decision i = m.add_decision(INT, OwnedValue::int_range(1, 3), "i");
		Decision v = m.add_decision(INT, OwnedValue::int_range(0, 99), "v");
		m.add_constraint("int_array_element", {list({num(10), num(20), num(30)}), num(1),
		                                       OwnedValue{i}, OwnedValue{v}});
		lin(m, "int_lin_eq", {1}, {OwnedValue{i}}, 2);
		Collected out = solve(lib, m, 2);
		check(out.solutions.size() == 1 && as_int(out.solutions[0][1]) == 20,
		      "int_array_element reads through its offset");
	}
}

/// An unsatisfiable model is `Complete` with no solution reported.
void unsat_model(fznso::Library& lib) {
	LayeredModel m;
	Decision x = m.add_decision(INT, OwnedValue::int_range(0, 10), "x");
	lin(m, "int_lin_le", {1}, {OwnedValue{x}}, 2);
	lin(m, "int_lin_le", {-1}, {OwnedValue{x}}, -5);
	Collected out = solve(lib, m, 1);
	check(out.status.complete(), "an unsatisfiable model completes");
	check(out.solutions.empty(), "an unsatisfiable model reports no solution");
}

/// A caller that has already asked to stop gets no search at all.
void stop_signal(fznso::Library& lib) {
	LayeredModel m;
	Decision x = m.add_decision(INT, OwnedValue::int_range(0, 10), "x");
	(void)x;
	fznso::DynSolver solver = lib.create_solver();
	std::vector<int> seen;
	Status status = solver.run(
		m, [&](const Solution&) { seen.push_back(1); },
		[](std::string_view, fznso::Value) {}, []() { return true; });
	check(!status.complete(), "a stopped run is incomplete");
	check(seen.empty(), "a stopped run reports no solution");
}

/// Options round-trip, and a bad one is rejected with a message.
void options(fznso::Library& lib) {
	fznso::DynSolver solver = lib.create_solver();
	check(!solver.option_set("time_limit", fznso::Value{std::int64_t{5000}}).has_value(),
	      "time_limit accepted");
	check(solver.option_get("time_limit").as_int() == 5000, "time_limit reads back");
	check(solver.option_set("time_limit", fznso::Value{std::int64_t{0}}).has_value(),
	      "a non-positive time_limit is rejected");
	check(solver.option_get("time_limit").as_int() == 5000,
	      "a rejected time_limit leaves the old value");
	check(solver.option_set("nonesuch", fznso::Value{true}).has_value(),
	      "an unknown option is rejected");
	check(solver.option_set("z3_float_encoding", fznso::Value{std::string{"nonesuch"}})
	          .has_value(),
	      "an unknown float encoding is rejected");
	check(!solver.option_set("z3_float_encoding", fznso::Value{std::string{"real"}}).has_value(),
	      "the real float encoding is accepted");
	check(solver.option_get("z3_float_encoding").as_string() == "real",
	      "the float encoding reads back");
}

/// Every constraint that has no decomposition is either declared here, or
/// shadowed by `fznso_nosets/`, or on the recorded exception list.
///
/// `fznso_constraints/` holds two very different kinds of `abort`. Most are the
/// standard library's own refusals, carried across verbatim — MiniZinc has never
/// reified a `graph_path`, and that is not this solver's problem. The ones that
/// *are* this solver's problem say so in their message: a FlatZinc builtin backs
/// the constraint, so no body could exist, and **a model using one fails outright
/// unless the solver declares it**.
///
/// Deriving the list from the library directory rather than hard-coding it is
/// the point: a future `fznso_constraints/` that adds one is caught here rather
/// than in a model somebody happens to run.
void abort_coverage(fznso::Library& lib, const char* stdlib) {
	namespace fs = std::filesystem;
	fs::path root{stdlib};
	if (!fs::is_directory(root / "fznso_constraints")) {
		std::fprintf(stderr, "skipping abort coverage: no MiniZinc library at %s\n", stdlib);
		return;
	}

	// Constraints Z3 cannot implement and nothing can decompose. Each is a
	// deliberate decision, not an oversight — see the solver's README.
	const std::set<std::string> exceptions = {
		// No theory in Z3, and no decomposition anywhere: these are primitives.
		"float_exp", "float_ln", "float_log2", "float_log10", "float_sin", "float_cos",
		"float_tan", "float_asin", "float_acos", "float_atan", "float_sinh", "float_cosh",
		"float_tanh", "float_asinh", "float_acosh", "float_atanh",
		// The registry types the exponent `var float`; an integer exponent is a
		// different type rather than a narrowing of it, and neither encoding has
		// a genuine float power.
		"float_pow",
		// Optional-task scheduling: needs introduced variables, which is library
		// work rather than a formula.
		"int_alternative", "int_alternative_reif",
		// Its extrema range over the *present* sub-tasks, and the arguments it
		// is declared with carry no `opt` flag to tell one from another.
		"int_span",
	};

	std::set<std::string> declared;
	FznsoConstraintList list_ = lib.constraint_types();
	for (std::size_t i = 0; i < list_.len; i++) {
		const FznsoStr& id = list_.constraints[i].ident;
		declared.insert(std::string{id.ptr, id.len});
	}
	std::set<std::string> shadowed;
	for (const fs::directory_entry& e : fs::directory_iterator{root / "fznso_nosets"}) {
		std::string name = e.path().stem().string();
		if (name.rfind("fzn_", 0) == 0) {
			shadowed.insert(name.substr(4));
		}
	}

	std::size_t total = 0;
	std::vector<std::string> uncovered;
	for (const fs::directory_entry& e : fs::directory_iterator{root / "fznso_constraints"}) {
		if (e.path().extension() != ".mzn") {
			continue;
		}
		std::ifstream in{e.path()};
		std::string body{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
		if (body.find("neither implements the constraint nor decomposes it") == std::string::npos) {
			continue;
		}
		total++;
		std::string name = e.path().stem().string().substr(4);
		if (declared.count(name) == 0 && shadowed.count(name) == 0 && exceptions.count(name) == 0) {
			uncovered.push_back(name);
		}
	}

	check(total > 0, "found the constraints that have no decomposition");
	for (const std::string& name : uncovered) {
		std::fprintf(stderr, "FAIL: `%s' has no decomposition and is neither declared nor a "
		                     "recorded exception\n",
		             name.c_str());
		failures++;
	}
	// The exception list must not rot either: a name on it that has since gained
	// a decomposition is a claim that is no longer true.
	for (const std::string& name : exceptions) {
		if (declared.count(name) != 0) {
			std::fprintf(stderr, "FAIL: `%s' is on the exception list but is declared\n",
			             name.c_str());
			failures++;
		}
	}
}

/// `intermediate` asks for each improving solution as it is found, not just the
/// last. Z3's optimiser only hands back the optimum, so this rides a model
/// callback — and the thing worth checking is that the reported sequence really
/// does improve and does not repeat the optimum at the end.
void intermediate(fznso::Library& lib) {
	LayeredModel m;
	Decision x = m.add_decision(INT, OwnedValue::int_range(0, 40), "x");
	Decision y = m.add_decision(INT, OwnedValue::int_range(0, 40), "y");
	// A knapsack-ish model with enough structure that the optimiser improves in
	// steps rather than landing on the answer immediately.
	lin(m, "int_lin_le", {3, 5}, {OwnedValue{x}, OwnedValue{y}}, 97);
	lin(m, "int_lin_le", {1, -1}, {OwnedValue{x}, OwnedValue{y}}, 4);
	m.set_objective("int_maximize", OwnedValue{y});

	Collected plain = solve(lib, m, 2);
	check(plain.status.complete() && plain.solutions.size() == 1,
	      "without `intermediate` only the optimum is reported");

	Collected out = solve(lib, m, 2, {{"intermediate", OwnedValue{true}}});
	check(out.status.complete(), "intermediate run completes");
	check(!out.solutions.empty(), "intermediate run reports at least one solution");
	if (out.solutions.empty() || plain.solutions.empty()) {
		return;
	}
	// Whatever the sequence, it must be strictly improving and must end at the
	// same optimum the plain run found.
	for (std::size_t i = 1; i < out.solutions.size(); i++) {
		check(as_int(out.solutions[i][1]) > as_int(out.solutions[i - 1][1]),
		      "intermediate solutions are reported in improving order");
	}
	check(as_int(out.solutions.back()[1]) == as_int(plain.solutions.back()[1]),
	      "the intermediate sequence ends at the optimum");
}

/// A run that fails part-way must leave the solver exactly as it found it.
///
/// Posting a layer pushes a Z3 scope before it knows whether every constraint in
/// it can be posted. If one cannot, that scope is one level of assertions the
/// solver's own bookkeeping does not know about — and the *next* run would post
/// on top of it and answer against constraints from a model that was rejected.
/// Only reusing the instance after a failure shows it.
void recovers_after_failure(fznso::Library& lib) {
	fznso::DynSolver solver = lib.create_solver();

	// A model whose second constraint this solver does not implement, so the
	// first one is posted and then the layer is abandoned.
	LayeredModel bad;
	Decision x = bad.add_decision(INT, OwnedValue::int_range(0, 10), "x");
	lin(bad, "int_lin_le", {-1}, {OwnedValue{x}}, -7); // x >= 7
	bad.add_constraint("int_nvalue", {list({OwnedValue{x}}), OwnedValue{x}});

	Collected failed = solve(solver, bad, 1);
	check(failed.status.failed(), "the run with an unimplemented constraint fails");

	// The same instance, now given a model that should have every value of
	// `0..10` available. If `x >= 7` survived from the abandoned layer, the
	// minimum comes back as 7 instead of 0.
	LayeredModel good;
	// Named `x` like the abandoned model's decision, and at the same index, so
	// it becomes the *same* Z3 constant. That is what makes a leaked assertion
	// observable rather than merely present.
	Decision y = good.add_decision(INT, OwnedValue::int_range(0, 10), "x");
	good.set_objective("int_minimize", OwnedValue{y});
	Collected out = solve(solver, good, 1);
	check(out.status.complete() && out.solutions.size() == 1,
	      "the next run on the same instance succeeds");
	if (!out.solutions.empty()) {
		check(as_int(out.solutions[0][0]) == 0,
		      "nothing survives from the abandoned layer");
	}
}

/// `all_solutions` under a lexicographic objective means every solution tying on
/// *all* of the objectives, not just the first.
///
/// Pinning only the leading objective would report `(0,4)` and `(0,5)` below,
/// which match on `a` and are worse on `b` — not ties at all.
void lex_all_solutions(fznso::Library& lib) {
	LayeredModel m;
	Decision a = m.add_decision(INT, OwnedValue::int_range(0, 5), "a");
	Decision b = m.add_decision(INT, OwnedValue::int_range(0, 5), "b");
	lin(m, "int_lin_le", {-1, -1}, {OwnedValue{a}, OwnedValue{b}}, -3); // a + b >= 3
	m.set_objective("int_lex_minimize", list({OwnedValue{a}, OwnedValue{b}}));

	Collected out = solve(lib, m, 2, {{"all_solutions", OwnedValue{true}}});
	check(out.status.complete(), "lexicographic all_solutions completes");
	check(out.solutions.size() == 1,
	      "only the one lexicographically optimal assignment ties");
	for (const std::vector<OwnedValue>& s : out.solutions) {
		check(as_int(s[0]) == 0 && as_int(s[1]) == 3,
		      "every reported solution is the lexicographic optimum");
	}
}

/// `all_solutions` with a single objective: every solution tied with the optimum,
/// and no worse one.
void single_all_solutions(fznso::Library& lib) {
	LayeredModel m;
	Decision a = m.add_decision(INT, OwnedValue::int_range(0, 3), "a");
	Decision b = m.add_decision(INT, OwnedValue::int_range(0, 3), "b");
	// Maximising `a + b` over `a + b <= 3` ties on every assignment summing to 3.
	lin(m, "int_lin_le", {1, 1}, {OwnedValue{a}, OwnedValue{b}}, 3);
	m.set_objective("int_maximize", OwnedValue{a});
	// `a` alone is maximised, so the ties are `a = 3` with `b` free below 1.
	Collected out = solve(lib, m, 2, {{"all_solutions", OwnedValue{true}}});
	check(out.status.complete(), "single-objective all_solutions completes");
	check(out.solutions.size() == 1, "one assignment ties at a = 3");
	for (const std::vector<OwnedValue>& s : out.solutions) {
		check(as_int(s[0]) == 3, "every reported solution attains the optimum");
	}
}

/// An identifier the solver never declared must fail, naming itself.
void unknown_constraint(fznso::Library& lib) {
	LayeredModel m;
	Decision x = m.add_decision(INT, OwnedValue::int_range(0, 3), "x");
	m.add_constraint("int_nvalue", {list({OwnedValue{x}}), OwnedValue{x}});
	Collected out = solve(lib, m, 1);
	check(out.status.failed(), "an undeclared constraint fails");
	check(out.status.error.find("int_nvalue") != std::string::npos,
	      "the failure names the constraint");
}

} // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <solver library> [minizinc library dir]\n", argv[0]);
		return 2;
	}
	fznso::Library lib{argv[1]};

	int_model(lib);
	unbounded_int(lib);
	unbounded_objective(lib);
	integer_division(lib);
	reification(lib);
	layers(lib);
	permanent_layers(lib);
	replaced_permanent_layer(lib);
	lex_objective(lib);
	pareto_objective(lib);
	lex_all_solutions(lib);
	single_all_solutions(lib);
	float_model(lib);
	float_objective(lib);
	globals(lib);
	unsat_model(lib);
	stop_signal(lib);
	options(lib);
	intermediate(lib);
	unknown_constraint(lib);
	recovers_after_failure(lib);
	if (argc >= 3) {
		abort_coverage(lib, argv[2]);
	}

	if (failures != 0) {
		std::fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	std::printf("all checks passed\n");
	return 0;
}
