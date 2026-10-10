// The FZnSO solver, and the state it keeps between runs.
//
// The state is the point. An instance is created, configured, run any number of
// times and freed, and what survives a run here is the Z3 assertion stack: one
// scope per model layer, so that solve → add a constraint → solve again re-posts
// only what is new. `layer_unchanged()` is what says how much of that is still
// valid.

#ifndef FZNSO_Z3_SOLVER_HH
#define FZNSO_Z3_SOLVER_HH

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <z3++.h>

#include "declarations.hh"
#include "fznso_export.hpp"
#include "translate.hh"

namespace fznso_z3 {

/// The options this solver accepts, held together so a run reads one thing.
struct Options {
	std::optional<std::int64_t> time_limit_ms;
	std::optional<std::int64_t> random_seed;
	bool verbose = false;
	bool intermediate = false;
	bool all_solutions = false;
	FloatEncoding float_encoding = FloatEncoding::Fpa;
};

class Z3Solver : public fznso::Solver {
public:
	Z3Solver();

	fznso::Value option_get(std::string_view name) const override;
	std::optional<std::string> option_set(std::string_view name, fznso::Value value) override;
	fznso::Value statistic(std::string_view name) const override;
	fznso::Status run(const fznso::Model& model, fznso::SolutionSink& solutions,
	                  fznso::MessageSink& messages, const fznso::StopSignal& stop) override;

	static FznsoConstraintList constraint_list() { return Declarations::get().constraints(); }
	static FznsoTypeList decision_list() { return Declarations::get().decisions(); }
	static FznsoObjectiveList objective_list() { return Declarations::get().objectives(); }
	static FznsoOptionList option_list() { return Declarations::get().options(); }
	static FznsoStatisticList statistic_list() { return Declarations::get().statistics(); }

private:
	/// Drop every layer, so the next run posts the model from scratch.
	void forget_layers();

	/// Replace the engine with a fresh one, dropping every assertion and every
	/// scope. This is how layer 0 is retracted: it is asserted at the base level
	/// and so has no scope of its own to be popped out of.
	void reset_engine();

	/// Bring the Z3 assertion stack into line with `model`'s layers, posting
	/// only what is above the last unchanged one. Returns how many layers were
	/// posted — zero meaning the whole model was reused.
	std::size_t post_layers(const fznso::Model& model, fznso::MessageSink& messages);

	/// Post one layer's decisions and constraints into the scope already pushed
	/// for it. Throws if a constraint cannot be posted; the caller pops.
	void post_layer(const fznso::Model& model, fznso::MessageSink& messages, std::size_t l);

	/// Hand out a borrowed value, keeping the payload alive in a node-based
	/// container so an earlier read survives a later one.
	fznso::Value cached(const std::string& name, fznso::OwnedValue value) const;

	// The engine the current layer stack lives in. `Z3_optimize` is only used
	// when the model has an objective, because its `check` is not incremental:
	// it allocates a fresh solver, re-imports every hard constraint and re-runs
	// its own preprocessing pipeline (`dt2bv`, `lia2card`, `eq2bv`) every time,
	// so a re-check of an unchanged model costs as much as the first one. A
	// `Z3_solver` keeps its state across `check`, which is what the layers are
	// for. Measured on one `gbac` instance, re-checking an unchanged model:
	// 213 ms through the optimiser against 12 ms through a solver, and 20
	// neighbourhoods of `reduced_UD4-gbac` in 1.4 s against 170.6 s.
	//
	// What this costs is worth saying out loud, because it is not preprocessing.
	// Throwing the solver away is also how the optimiser hands Z3 the whole
	// formula set in one batch, and the smt kernel is far better on these models
	// when it configures itself from everything at once than when it is fed one
	// assertion at a time: on `radiation/m12_10_20` the optimiser finds a first
	// solution in 9.4 s — 8.6 s with its preprocessing switched off entirely —
	// where a solver holding the same assertions finds none in 60 s at six
	// random seeds. Nothing incremental recovers that. See the README.
	void engine_push() { layers_in_opt_ ? opt_.push() : sol_.push(); }
	void engine_pop() { layers_in_opt_ ? opt_.pop() : sol_.pop(); }
	void engine_add(const z3::expr& e) { layers_in_opt_ ? opt_.add(e) : sol_.add(e); }
	z3::check_result engine_check() { return layers_in_opt_ ? opt_.check() : sol_.check(); }
	z3::model engine_model() { return layers_in_opt_ ? opt_.get_model() : sol_.get_model(); }

	z3::context ctx_;
	z3::optimize opt_;
	// `Z3_mk_simple_solver` rather than `Z3_mk_solver`: the latter is a combined
	// solver that runs the strategic tactic until the first `push` and then hands
	// over, which costs more than it saves in a workload that is incremental by
	// construction (2.1 s against 0.6 s per re-check on `UD2-gbac`).
	z3::solver sol_;
	bool layers_in_opt_ = false;
	Translate translate_;
	Options options_;

	// One Z3 scope per posted layer, and where each layer's decisions ended.
	// Global indices follow layer order, so a popped layer is a truncation.
	std::size_t posted_layers_ = 0;
	std::vector<std::size_t> decision_end_;
	// Per layer, whether it introduced a nonlinear real term. Kept per layer
	// rather than as one flag because popping a layer takes its nonlinearity
	// with it, and the next run's status depends on what is still posted.
	std::vector<bool> layer_nonlinear_;

	mutable std::map<std::string, fznso::OwnedValue> cache_;

	// Statistics of the last run.
	std::int64_t solutions_ = 0;
	std::int64_t layers_posted_ = 0;
	std::size_t constraints_ = 0;
	double init_time_ = 0.0;
	double solve_time_ = 0.0;
	bool have_objective_ = false;
	bool float_objective_ = false;
	fznso::OwnedValue objective_;
	fznso::OwnedValue bound_;
};

} // namespace fznso_z3

#endif // FZNSO_Z3_SOLVER_HH
