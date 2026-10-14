#pragma once

#include "Compiler/AbstractSyntaxTree/AbstractSyntaxTree.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// What a function hands to another worker: the parameters whose values reach
// one, and whether its result comes back from one. A call to the function hands
// its arguments in those places across, as a call to Spawn hands both of its.
struct WorkerCrossing
{
	std::vector<size_t> m_crossing_parameters;
	bool m_returns_from_worker = false;

	bool operator==(const WorkerCrossing&) const = default;
};

// By the name a call writes: this module's functions bare, an imported module's
// as `Module::name`.
using WorkerCrossings = std::unordered_map<std::string, WorkerCrossing>;

// What a call hands to another worker, from the primitive it is or the summary of
// the function it names; null when it hands nothing.
const WorkerCrossing* FindCallCrossing(const MidoriExpression::Call& call, const WorkerCrossings& crossings);

// The summaries of a checked module's top-level functions that hand anything
// across, given those of the modules it imports.
WorkerCrossings FindWorkerCrossings(MidoriProgramTree& program_tree, const WorkerCrossings& imported);

bool ContainsCell(const std::shared_ptr<MidoriType>& type);
