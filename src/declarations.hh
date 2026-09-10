// The five capability lists.
//
// Built once, into a function-local `static`, and handed out by the five list
// entry points. The `FznsoConstraintType`s point into this object's own storage,
// so it must outlive every use of the lists — which, being a `static`, it does.

#ifndef FZNSO_Z3_DECLARATIONS_HH
#define FZNSO_Z3_DECLARATIONS_HH

#include <deque>
#include <string>
#include <vector>

#include "fznso.hpp"

namespace fznso_z3 {

/// Argument type shorthands. `V` marks the decision-variable form, `A` the list.
constexpr FznsoType kB = fznso::Type{FznsoTypeBaseBool};
constexpr FznsoType kVB = fznso::Type{FznsoTypeBaseBool}.decision(true);
constexpr FznsoType kI = fznso::Type{FznsoTypeBaseInt};
constexpr FznsoType kVI = fznso::Type{FznsoTypeBaseInt}.decision(true);
constexpr FznsoType kF = fznso::Type{FznsoTypeBaseFloat};
constexpr FznsoType kVF = fznso::Type{FznsoTypeBaseFloat}.decision(true);
constexpr FznsoType kS = fznso::Type{FznsoTypeBaseInt}.set(true);
constexpr FznsoType kStr = fznso::Type{FznsoTypeBaseString};
constexpr FznsoType kOptI = fznso::Type{FznsoTypeBaseInt}.opt(true);
constexpr FznsoType kAB = fznso::Type{FznsoTypeBaseBool}.list(true);
constexpr FznsoType kAVB = fznso::Type{FznsoTypeBaseBool}.list(true).decision(true);
constexpr FznsoType kAI = fznso::Type{FznsoTypeBaseInt}.list(true);
constexpr FznsoType kAVI = fznso::Type{FznsoTypeBaseInt}.list(true).decision(true);
constexpr FznsoType kAF = fznso::Type{FznsoTypeBaseFloat}.list(true);
constexpr FznsoType kAVF = fznso::Type{FznsoTypeBaseFloat}.list(true).decision(true);

/// What this solver declares.
class Declarations {
public:
	Declarations();

	FznsoConstraintList constraints() const { return {constraints_.size(), constraints_.data()}; }
	FznsoTypeList decisions() const { return {decisions_.size(), decisions_.data()}; }
	FznsoObjectiveList objectives() const { return {objectives_.size(), objectives_.data()}; }
	FznsoOptionList options() const { return {options_.size(), options_.data()}; }
	FznsoStatisticList statistics() const { return {statistics_.size(), statistics_.data()}; }

	/// The one instance, built on first use.
	static const Declarations& get();

private:
	/// Declare a constraint exactly as named.
	void add(std::string ident, std::vector<FznsoType> args);
	/// Declare a constraint together with its `_reif` and `_imp` forms, which
	/// take one extra `var bool`.
	///
	/// Every non-functional registry entry implies both, and for this solver
	/// both are free: a constraint is one Boolean formula, so reifying it is
	/// `r == f` and half-reifying it is `r -> f`. There is no propagator to
	/// write, which is why so much more is declared here than by a MIP solver.
	void add_reified(const std::string& ident, std::vector<FznsoType> args,
	                 bool with_imp = true);

	// Deques rather than vectors: both hand out pointers into themselves, and a
	// deque never moves what it already holds.
	std::deque<std::string> names_;
	std::deque<std::vector<FznsoType>> arguments_;
	std::vector<FznsoConstraintType> constraints_;
	std::vector<FznsoType> decisions_;
	std::vector<FznsoObjective> objectives_;
	std::vector<FznsoOption> options_;
	std::vector<FznsoStatistic> statistics_;
};

} // namespace fznso_z3

#endif // FZNSO_Z3_DECLARATIONS_HH
