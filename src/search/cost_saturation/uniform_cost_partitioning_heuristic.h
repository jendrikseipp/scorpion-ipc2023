#ifndef COST_SATURATION_UNIFORM_COST_PARTITIONING_HEURISTIC_H
#define COST_SATURATION_UNIFORM_COST_PARTITIONING_HEURISTIC_H

#include "max_cost_partitioning_heuristic.h"

namespace cost_saturation {
class UniformCostPartitioningHeuristic : public MaxCostPartitioningHeuristic {
protected:
    virtual int compute_heuristic(const State &ancestor_state) override;

public:
    UniformCostPartitioningHeuristic(
        const options::Options &opts,
        Abstractions &&abstractions,
        CPHeuristics &&cp_heuristics);
};
}

#endif
