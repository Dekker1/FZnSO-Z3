#include "solver.hh"

#include <algorithm>
#include <chrono>
#include <functional>
#include <cmath>

namespace fznso_z3 {

namespace {

/// One solution's assignment, copied out of a Z3 model.
///
/// Everything handed across the interface is borrowed for the duration of the
/// callback, so the values are copied up front rather than read on demand.
class Solution final : public fznso::SolutionSource {
public:
	Solution(std::vector<fznso::OwnedValue> values, std::int64_t index, bool float_objective,
	         fznso::OwnedValue objective)
		: values_(std::move(values)),
		  index_(index),
		  objective_(std::move(objective)),
		  objective_name_(float_objective ? "float_objective" : "int_objective") {}

	fznso::Value value(fznso::Decision decision) const override {
		return decision.index < values_.size() ? fznso::Value{values_[decision.index]}
		                                       : fznso::Value{};
	}

	fznso::Value statistic(std::string_view name) const override {
		if (name == "solutions") {
			return fznso::Value{index_};
		}
		if (name == objective_name_) {
			return fznso::Value{objective_};
		}
		return fznso::Value{};
	}

private:
	std::vector<fznso::OwnedValue> values_;
	std::int64_t index_;
	fznso::OwnedValue objective_;
	std::string_view objective_name_;
};

double seconds_since(std::chrono::steady_clock::time_point start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

/// How a model's objective identifier combines its arguments.
enum class Mode { None, Single, Lex, Pareto };

struct Objective {
	Mode mode = Mode::None;
	bool maximise = false;
	bool floating = false;
};

/// Read the objective identifier. Returns absent for a name this solver did not
/// declare, which is a model error rather than something to guess at.
std::optional<Objective> classify(std::string_view ident) {
	if (ident.empty()) {
		return Objective{};
	}
	Objective o;
	o.floating = ident.compare(0, 6, "float_") == 0;
	std::string_view rest = ident.substr(o.floating ? 6 : 4);
	if (rest == "minimize" || rest == "maximize") {
		o.mode = Mode::Single;
	} else if (rest == "lex_minimize" || rest == "lex_maximize") {
		o.mode = Mode::Lex;
	} else if (rest == "pareto_maximize") {
		o.mode = Mode::Pareto;
	} else {
		return std::nullopt;
	}
	o.maximise = rest.substr(rest.size() - 8) == "maximize";
	return o;
}

/// Whether a Z3 objective bound is an actual number.
///
/// An unbounded objective comes back as `oo` or `-oo`, and one Z3 could not pin
/// down as an `epsilon` term — neither of which is a value to report.
bool is_number(const z3::expr& e) { return e.is_numeral(); }

} // namespace

Z3Solver::Z3Solver()
	: ctx_(), opt_(ctx_), translate_(ctx_, FloatEncoding::Fpa) {}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

fznso::Value Z3Solver::cached(const std::string& name, fznso::OwnedValue value) const {
	auto it = cache_.insert_or_assign(name, std::move(value)).first;
	return fznso::Value{it->second};
}

fznso::Value Z3Solver::option_get(std::string_view name) const {
	if (name == "all_solutions") {
		return cached("all_solutions", fznso::OwnedValue{options_.all_solutions});
	}
	if (name == "intermediate") {
		return cached("intermediate", fznso::OwnedValue{options_.intermediate});
	}
	if (name == "verbose") {
		return cached("verbose", fznso::OwnedValue{options_.verbose});
	}
	if (name == "time_limit") {
		return cached("time_limit", fznso::OwnedValue{options_.time_limit_ms});
	}
	if (name == "random_seed") {
		return cached("random_seed", fznso::OwnedValue{options_.random_seed});
	}
	if (name == "z3_float_encoding") {
		return cached("z3_float_encoding",
		              fznso::OwnedValue{options_.float_encoding == FloatEncoding::Fpa
		                                    ? std::string{"float"}
		                                    : std::string{"real"}});
	}
	return fznso::Value{};
}

std::optional<std::string> Z3Solver::option_set(std::string_view name, fznso::Value value) {
	auto reject = [&](const char* want) {
		return "option `" + std::string{name} + "' expects " + want;
	};
	if (name == "all_solutions" || name == "intermediate" || name == "verbose") {
		if (value.kind() != FznsoValueBool) {
			return reject("a boolean");
		}
		bool v = value.as_bool();
		if (name == "all_solutions") {
			options_.all_solutions = v;
		} else if (name == "intermediate") {
			options_.intermediate = v;
		} else {
			options_.verbose = v;
		}
		return std::nullopt;
	}
	if (name == "time_limit" || name == "random_seed") {
		std::optional<std::int64_t>& slot =
			name == "time_limit" ? options_.time_limit_ms : options_.random_seed;
		if (value.kind() == FznsoValueAbsent) {
			slot.reset();
			return std::nullopt;
		}
		if (value.kind() != FznsoValueInt) {
			return reject("an integer or the absent value");
		}
		// The type carries no range, so a non-positive budget is rejected here.
		if (name == "time_limit" && value.as_int() <= 0) {
			return "option `time_limit' must be positive, got " + std::to_string(value.as_int());
		}
		slot = value.as_int();
		return std::nullopt;
	}
	if (name == "z3_float_encoding") {
		if (value.kind() != FznsoValueString) {
			return reject("the string `float' or `real'");
		}
		std::string_view v = value.as_string();
		FloatEncoding chosen;
		if (v == "float") {
			chosen = FloatEncoding::Fpa;
		} else if (v == "real") {
			chosen = FloatEncoding::Real;
		} else {
			return "option `z3_float_encoding' expects `float' or `real', got `" +
			       std::string{v} + "'";
		}
		if (chosen != options_.float_encoding) {
			// The encoding decides the sort of every float decision, so the
			// variables held from previous runs cannot survive it. Dropping the
			// layers is the honest answer; keeping them would mean posting new
			// constraints against variables of the old sort.
			options_.float_encoding = chosen;
			forget_layers();
		}
		return std::nullopt;
	}
	return "this solver has no option named `" + std::string{name} + "'";
}

fznso::Value Z3Solver::statistic(std::string_view name) const {
	if (name == "solutions") {
		return cached("solutions", fznso::OwnedValue{solutions_});
	}
	if (name == "z3_layers_posted") {
		return cached("z3_layers_posted", fznso::OwnedValue{layers_posted_});
	}
	if (name == "decisions") {
		return cached("decisions",
		              fznso::OwnedValue{static_cast<std::int64_t>(translate_.size())});
	}
	if (name == "constraints") {
		return cached("constraints", fznso::OwnedValue{static_cast<std::int64_t>(constraints_)});
	}
	if (name == "init_time") {
		return cached("init_time", fznso::OwnedValue{init_time_});
	}
	if (name == "solve_time") {
		return cached("solve_time", fznso::OwnedValue{solve_time_});
	}
	if (have_objective_) {
		if (name == (float_objective_ ? "float_objective" : "int_objective")) {
			return cached(std::string{name}, objective_);
		}
		if (name == (float_objective_ ? "float_objective_bound" : "int_objective_bound")) {
			return cached(std::string{name}, bound_);
		}
	}
	// Z3's own counters, under this solver's prefix because they are not
	// registry names. Read by the name Z3 uses, which is not always ours.
	static const std::pair<std::string_view, const char*> kZ3[] = {
		{"z3_conflicts", "conflicts"},
		{"z3_decisions", "decisions"},
		{"z3_propagations", "propagations"},
		{"z3_memory", "memory"},
	};
	for (const auto& entry : kZ3) {
		if (name != entry.first) {
			continue;
		}
		z3::stats s = opt_.statistics();
		for (unsigned i = 0; i < s.size(); i++) {
			if (s.key(i) != entry.second) {
				continue;
			}
			return s.is_uint(i)
			         ? cached(std::string{name},
			                  fznso::OwnedValue{static_cast<std::int64_t>(s.uint_value(i))})
			         : cached(std::string{name}, fznso::OwnedValue{s.double_value(i)});
		}
		return fznso::Value{};
	}
	return fznso::Value{};
}

// ---------------------------------------------------------------------------
// Layers
// ---------------------------------------------------------------------------

void Z3Solver::forget_layers() {
	while (posted_layers_ > 0) {
		opt_.pop();
		posted_layers_--;
	}
	decision_end_.clear();
	layer_nonlinear_.clear();
	translate_.reset(options_.float_encoding);
}

std::size_t Z3Solver::post_layers(const fznso::Model& model, fznso::MessageSink& messages) {
	const std::size_t layers = model.layer_count();

	// Layers at or above `unchanged` may have been replaced, so their scopes are
	// popped and rebuilt. Only `unchanged` promises the solver has seen a layer as
	// it is now: `permanent` is no floor, because a consumer can pop a layer, push
	// a different one and mark it permanent before the next run. Clamping by
	// `posted_layers_` and `layers` is what makes *popping* free — a model that
	// shrank drops the surplus scopes and posts nothing.
	const std::size_t keep = std::min(model.layer_unchanged(), std::min(posted_layers_, layers));

	while (posted_layers_ > keep) {
		opt_.pop();
		posted_layers_--;
	}
	// A Z3 constant outlives the scope its assertions were made in, so this is
	// what actually forgets a popped layer's decisions. Global indices follow
	// layer order, which is why a truncation is enough.
	translate_.truncate(keep == 0 ? 0 : decision_end_[keep - 1]);
	decision_end_.resize(keep);
	layer_nonlinear_.resize(keep);

	std::size_t posted = 0;
	for (std::size_t l = keep; l < layers; l++) {
		opt_.push();
		// From here to the end of the iteration the Z3 scope stack is one deeper
		// than `posted_layers_` records. Anything that throws in between — an
		// identifier this solver does not implement is the ordinary case — would
		// otherwise leave that scope behind holding half a layer, and the next
		// run would post on top of it. Popping it here is what keeps the two in
		// step, so a failed run leaves the solver exactly as it found it.
		try {
			post_layer(model, messages, l);
		} catch (...) {
			opt_.pop();
			throw;
		}
		decision_end_.push_back(model.decision_layer_end(l));
		layer_nonlinear_.push_back(translate_.saw_nonlinear_real());
		posted_layers_ = l + 1;
		posted++;
	}
	if (posted == 0 && messages.wanted()) {
		std::string line = "reused all " + std::to_string(layers) + " layer(s)";
		messages.message("log", fznso::Value{line});
	}
	constraints_ = layers == 0 ? 0 : model.constraint_layer_end(layers - 1);
	return posted;
}

void Z3Solver::post_layer(const fznso::Model& model, fznso::MessageSink& messages,
                          std::size_t l) {
	{
		translate_.clear_nonlinear_real();
		std::size_t first_decision = l == 0 ? 0 : model.decision_layer_end(l - 1);
		std::size_t last_decision = model.decision_layer_end(l);
		std::size_t first_constraint = l == 0 ? 0 : model.constraint_layer_end(l - 1);
		std::size_t last_constraint = model.constraint_layer_end(l);

		for (std::size_t d = first_decision; d < last_decision; d++) {
			std::optional<z3::expr> domain = translate_.add_decision(model, fznso::Decision{d});
			if (domain.has_value()) {
				opt_.add(*domain);
			}
		}
		for (std::size_t c = first_constraint; c < last_constraint; c++) {
			opt_.add(translate_.assertion(model, fznso::Constraint{c}));
		}

		if (messages.wanted()) {
			std::string line = "posted layer " + std::to_string(l) + ": " +
			                   std::to_string(last_decision - first_decision) + " decision(s), " +
			                   std::to_string(last_constraint - first_constraint) +
			                   " constraint(s)";
			messages.message("log", fznso::Value{line});
		}
	}
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

fznso::Status Z3Solver::run(const fznso::Model& model, fznso::SolutionSink& solutions,
                            fznso::MessageSink& messages, const fznso::StopSignal& stop) {
	using Kind_ = fznso::Status::Kind;
	// Polled before starting, so a caller that has already asked to stop gets no
	// search at all.
	if (stop.requested()) {
		return fznso::Status{Kind_::Incomplete, {}};
	}

	auto started = std::chrono::steady_clock::now();
	solutions_ = 0;
	objective_ = fznso::OwnedValue{};
	bound_ = fznso::OwnedValue{};

	std::optional<Objective> objective = classify(model.objective_ident());
	if (!objective.has_value()) {
		return fznso::Status{Kind_::Error, "unsupported objective `" +
		                                       std::string{model.objective_ident()} + "'"};
	}
	have_objective_ = objective->mode != Mode::None;
	float_objective_ = objective->floating;

	try {
		layers_posted_ = static_cast<std::int64_t>(post_layers(model, messages));
	} catch (const std::exception& e) {
		// A layer that failed to post left the scope stack in a state that no
		// longer matches the model, so the next run starts clean.
		forget_layers();
		return fznso::Status{Kind_::Error, e.what()};
	}
	init_time_ = seconds_since(started);

	// Which decisions need a value, resolved once rather than per solution.
	const std::size_t decisions = model.decision_count();
	std::vector<std::size_t> wanted;
	for (std::size_t d = 0; d < decisions; d++) {
		if (model.decision_in_solution(fznso::Decision{d})) {
			wanted.push_back(d);
		}
	}

	z3::params params(ctx_);
	if (options_.time_limit_ms.has_value()) {
		params.set("timeout", static_cast<unsigned>(*options_.time_limit_ms));
	}
	if (options_.random_seed.has_value()) {
		params.set("random_seed", static_cast<unsigned>(*options_.random_seed));
	}
	if (objective->mode == Mode::Pareto) {
		params.set("priority", ctx_.str_symbol("pareto"));
	} else if (objective->mode == Mode::Lex) {
		params.set("priority", ctx_.str_symbol("lex"));
	}
	opt_.set(params);

	// Everything a *run* adds — the objective, its side conditions, and the
	// blocking clauses of an enumeration — goes in one scope of its own, so that
	// none of it leaks into the layer state the next run reuses.
	opt_.push();
	struct PopGuard {
		z3::optimize& opt;
		~PopGuard() { opt.pop(); }
	} guard{opt_};

	auto solve_started = std::chrono::steady_clock::now();
	bool stopped = false;
	std::string error;

	auto report = [&](const z3::model& m, fznso::OwnedValue value) {
		std::vector<fznso::OwnedValue> assignment(decisions);
		for (std::size_t d : wanted) {
			assignment[d] = translate_.read(m, d);
		}
		objective_ = value;
		Solution src{std::move(assignment), solutions_, float_objective_, std::move(value)};
		solutions_++;
		solutions.solution(src);
		// Polled again here, which is what makes "stop after this solution"
		// reliable.
		stopped = stopped || stop.requested();
	};

	try {
		std::vector<z3::optimize::handle> handles;
		z3::expr_vector terms(ctx_);
		if (have_objective_) {
			z3::expr_vector guards(ctx_);
			fznso::Value arg = model.objective_arg();
			auto add = [&](fznso::Value v) {
				z3::expr term = translate_.objective_term(v, guards);
				terms.push_back(term);
				handles.push_back(objective->maximise ? opt_.maximize(term) : opt_.minimize(term));
			};
			if (objective->mode == Mode::Single) {
				add(arg);
			} else {
				if (arg.kind() != FznsoValueList) {
					throw std::runtime_error(
						"a lexicographic or Pareto objective takes a list of variables");
				}
				for (std::size_t i = 0; i < arg.size(); i++) {
					add(arg[i]);
				}
			}
			if (!guards.empty()) {
				// Under the FloatingPoint encoding an objective is carried by
				// an order-preserving bit-vector key, and NaN has no place in
				// that order. Excluding it is a constraint this solver adds of
				// its own, so it is said out loud rather than done quietly.
				opt_.add(z3::mk_and(guards));
				if (messages.wanted()) {
					std::string line =
						"float objective: NaN is excluded, because it has no place in the "
						"ordering an objective is optimised over";
					messages.message("warn", fznso::Value{line});
				}
			}
		}

		// `intermediate` asks for each improving solution as it is found, and
		// `optimize::check` only ever hands back the last one. Z3 will call a
		// handler as it improves, though, filling in a model object registered
		// up front — so the reporting happens from inside `check`, on the same
		// thread, and the final solution is skipped below if it was the last
		// one streamed.
		//
		// `streaming` must outlive the `check` call, which is why it is
		// declared here rather than in the branch that registers it.
		z3::model streaming(ctx_);
		struct Streamer {
			Z3Solver* self;
			z3::model* model;
			const std::function<void(const z3::model&)>* report;
			bool any = false;
		} streamer{this, &streaming, nullptr, false};
		std::function<void(const z3::model&)> stream_one = [&](const z3::model& m) {
			fznso::OwnedValue value;
			fznso::Value arg = model.objective_arg();
			if (arg.kind() == FznsoValueDecision) {
				value = translate_.read(m, arg.as_decision().index);
			}
			report(m, value);
		};
		streamer.report = &stream_one;
		if (options_.intermediate && have_objective_ && objective->mode == Mode::Single) {
			Z3_optimize_register_model_eh(
				ctx_, opt_, streaming, &streamer, [](void* p) noexcept {
					auto* s = static_cast<Streamer*>(p);
					s->any = true;
					(*s->report)(*s->model);
				});
		}

		// A satisfaction problem reports every solution it finds; an
		// optimisation problem reports the optimum, and the ones tied with it
		// when `all_solutions` asks. Pareto is neither: it reports a set of
		// incomparable solutions, one per check, which is what the registry
		// entry for it says.
		if (objective->mode == Mode::Pareto) {
			while (!stopped && opt_.check() == z3::sat) {
				report(opt_.get_model(), fznso::OwnedValue{});
			}
		} else {
			z3::check_result result = opt_.check();
			if (result == z3::unknown && !stop.requested()) {
				// A limit, not a failure: the search stopped without proving
				// anything either way.
				stopped = true;
			}
			if (result == z3::sat) {
				z3::model found = opt_.get_model();
				fznso::OwnedValue value;
				if (objective->mode == Mode::Single && !handles.empty()) {
					z3::expr bound = objective->maximise ? opt_.upper(handles.front())
					                                     : opt_.lower(handles.front());
					if (terms[0].is_bv()) {
						// A float objective under the FloatingPoint encoding
						// rides an order-preserving key, so this bound is that
						// key rather than a float — and a 64-bit key is always
						// finite, so there is no unboundedness to detect.
					} else if (!is_number(bound)) {
						// `oo`, or an `epsilon` term Z3 could not pin down.
						// Either way there is no optimum to report.
						throw std::runtime_error("the objective is unbounded");
					} else {
						bound_ = float_objective_
						           ? fznso::OwnedValue{bound.as_double()}
						           : fznso::OwnedValue{bound.as_int64()};
					}
					// Read the value from the solution rather than from the
					// bound: it is the one place both encodings agree, and it
					// is what "the objective value of the current solution"
					// means.
					fznso::Value arg = model.objective_arg();
					value = arg.kind() == FznsoValueDecision
					          ? translate_.read(found, arg.as_decision().index)
					          : fznso::OwnedValue{arg};
				}
				// The streamer reports each improving solution as it is found,
				// and the last of those *is* the optimum, so reporting it again
				// here would repeat it — and a repeat is not an improvement,
				// which is what the improving order is supposed to mean.
				if (!streamer.any) {
					report(found, value);
				} else {
					objective_ = value;
				}

				// `all_solutions` means every solution of a satisfaction
				// problem, and every solution *tied with the optimum* of an
				// optimisation one. Without it a single answer is what the
				// caller asked for — which is also how MiniZinc drives this,
				// lifting the option itself when it wants more.
				// ponytail: blocking-clause enumeration, so listing N solutions
				// costs N checks against a formula that grows by a clause each
				// time. Fine for the single answer MiniZinc asks for by default
				// and for a test suite's `-a`; if enumerating thousands ever
				// matters, the shape to reach for is Z3's own model iteration.
				if (options_.all_solutions) {
					// Pin the objective before enumerating. A blocking clause
					// on its own would let the next check settle for a worse
					// solution, which is not a tie.
					//
					// *Every* objective, not just the first: under a
					// lexicographic objective a tie means all of them are
					// equal, and pinning only the first would report solutions
					// that match on it and are worse on the rest.
					for (std::size_t o = 0; o < handles.size(); o++) {
						z3::expr optimal = objective->maximise ? opt_.upper(handles[o])
						                                      : opt_.lower(handles[o]);
						opt_.add(terms[static_cast<unsigned>(o)] == optimal);
					}
					// The model is carried along rather than re-fetched: adding
					// an assertion invalidates whatever the last `check` left
					// behind, and the pinning above is exactly such an addition.
					// Asking Z3 for it again here fails with "model is not
					// available".
					z3::model current = found;
					while (!stopped) {
						opt_.add(translate_.blocking_clause(current, wanted));
						if (opt_.check() != z3::sat) {
							break;
						}
						current = opt_.get_model();
						report(current, value);
					}
				}
			}
		}
	} catch (const z3::exception& e) {
		error = e.msg();
	} catch (const std::exception& e) {
		error = e.what();
	}

	solve_time_ = seconds_since(solve_started);
	if (!error.empty()) {
		return fznso::Status{Kind_::Error, std::move(error)};
	}
	if (stopped || stop.requested()) {
		return fznso::Status{Kind_::Incomplete, {}};
	}
	// `Complete` with an objective is a claim that the last solution reported is
	// *optimal*, and over nonlinear real arithmetic Z3 cannot support that: it
	// answers `x*x + y*y <= 16, maximize x + y` with 723/128 against a true
	// optimum of 4*sqrt(2), and reports `lower == upper` as though it had
	// proved it. The solutions are still valid, so this is `Incomplete` — the
	// status that says exactly "these answers hold, better ones may exist" —
	// rather than an error.
	bool unproven = have_objective_ &&
	                std::find(layer_nonlinear_.begin(), layer_nonlinear_.end(), true) !=
	                    layer_nonlinear_.end();
	if (unproven) {
		if (messages.wanted()) {
			std::string line =
				"optimality is not proven: Z3's optimiser is not exact over nonlinear real "
				"arithmetic, so the objective reported may not be the best one";
			messages.message("warn", fznso::Value{line});
		}
		return fznso::Status{Kind_::Incomplete, {}};
	}
	return fznso::Status{Kind_::Complete, {}};
}

} // namespace fznso_z3

FZNSO_EXPORT_SOLVER(fznso_z3::Z3Solver, z3);
