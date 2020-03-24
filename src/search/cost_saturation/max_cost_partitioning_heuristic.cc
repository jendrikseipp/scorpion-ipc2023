#include "max_cost_partitioning_heuristic.h"

#include "abstraction.h"
#include "cost_partitioning_heuristic.h"
#include "utils.h"

#include "../utils/logging.h"

using namespace std;

namespace cost_saturation {
static void log_info_about_stored_lookup_tables(
    const Abstractions &abstractions,
    const vector<CostPartitioningHeuristic> &cp_heuristics) {
    int num_abstractions = abstractions.size();

    // Print statistics about the number of lookup tables.
    int num_lookup_tables = num_abstractions * cp_heuristics.size();
    int num_stored_lookup_tables = 0;
    for (const auto &cp_heuristic: cp_heuristics) {
        num_stored_lookup_tables += cp_heuristic.get_num_lookup_tables();
    }
    utils::Log() << "Stored lookup tables: " << num_stored_lookup_tables << "/"
                 << num_lookup_tables << " = "
                 << num_stored_lookup_tables / static_cast<double>(num_lookup_tables)
                 << endl;

    // Print statistics about the number of stored values.
    int num_stored_values = 0;
    for (const auto &cp_heuristic : cp_heuristics) {
        num_stored_values += cp_heuristic.get_num_heuristic_values();
    }
    int num_total_values = 0;
    for (const auto &abstraction : abstractions) {
        num_total_values += abstraction->get_num_states();
    }
    num_total_values *= cp_heuristics.size();
    utils::Log() << "Stored values: " << num_stored_values << "/"
                 << num_total_values << " = "
                 << num_stored_values / static_cast<double>(num_total_values) << endl;
}

static AbstractionFunctions extract_abstraction_functions_from_useful_abstractions(
    const vector<CostPartitioningHeuristic> &cp_heuristics,
    const UnsolvabilityHeuristic &unsolvability_heuristic,
    Abstractions &abstractions) {
    int num_abstractions = abstractions.size();

    // Collect IDs of useful abstractions.
    vector<bool> useful_abstractions(num_abstractions, false);
    unsolvability_heuristic.mark_useful_abstractions(useful_abstractions);
    for (const auto &cp_heuristic : cp_heuristics) {
        cp_heuristic.mark_useful_abstractions(useful_abstractions);
    }

    AbstractionFunctions abstraction_functions;
    abstraction_functions.reserve(num_abstractions);
    for (int i = 0; i < num_abstractions; ++i) {
        if (useful_abstractions[i]) {
            abstraction_functions.push_back(
                abstractions[i]->extract_abstraction_function());
        } else {
            abstraction_functions.push_back(nullptr);
        }
    }
    return abstraction_functions;
}

MaxCostPartitioningHeuristic::MaxCostPartitioningHeuristic(
    const options::Options &opts,
    Abstractions abstractions,
    vector<CostPartitioningHeuristic> &&cp_heuristics_,
    UnsolvabilityHeuristic &&unsolvability_heuristic_)
    : Heuristic(opts),
      cp_heuristics(move(cp_heuristics_)),
      unsolvability_heuristic(move(unsolvability_heuristic_)) {
    log_info_about_stored_lookup_tables(abstractions, cp_heuristics);

    // We only need abstraction functions during search and no transition systems.
    abstraction_functions = extract_abstraction_functions_from_useful_abstractions(
        cp_heuristics, unsolvability_heuristic, abstractions);

    int num_abstractions = abstractions.size();
    int num_useless_abstractions = count(
        abstraction_functions.begin(), abstraction_functions.end(), nullptr);
    int num_useful_abstractions = num_abstractions - num_useless_abstractions;
    utils::Log() << "Useful abstractions: " << num_useful_abstractions << "/"
                 << num_abstractions << " = "
                 << static_cast<double>(num_useful_abstractions) / num_abstractions
                 << endl;

    compute_heuristic_timer = utils::make_unique_ptr<utils::Timer>(false);
    convert_global_state_timer = utils::make_unique_ptr<utils::Timer>(false);
    get_abstract_state_ids_timer = utils::make_unique_ptr<utils::Timer>(false);
    unsolvability_heuristic_timer = utils::make_unique_ptr<utils::Timer>(false);
    compute_max_h_timer = utils::make_unique_ptr<utils::Timer>(false);
}

MaxCostPartitioningHeuristic::~MaxCostPartitioningHeuristic() {
    print_statistics();
}

int MaxCostPartitioningHeuristic::compute_heuristic(const GlobalState &global_state) {
    convert_global_state_timer->resume();
    State state = convert_global_state(global_state);
    convert_global_state_timer->stop();
    return compute_heuristic(state);
}

int MaxCostPartitioningHeuristic::compute_heuristic(const State &state) const {
    compute_heuristic_timer->resume();

    get_abstract_state_ids_timer->resume();
    vector<int> abstract_state_ids = get_abstract_state_ids(
        abstraction_functions, state);
    get_abstract_state_ids_timer->stop();

    unsolvability_heuristic_timer->resume();
    if (unsolvability_heuristic.is_unsolvable(abstract_state_ids)) {
        unsolvability_heuristic_timer->stop();
        compute_heuristic_timer->stop();
        return DEAD_END;
    }
    unsolvability_heuristic_timer->stop();

    compute_max_h_timer->resume();
    int max_h = compute_max_h_with_statistics(cp_heuristics, abstract_state_ids, num_best_order);
    compute_max_h_timer->stop();

    compute_heuristic_timer->stop();
    return max_h;
}

void MaxCostPartitioningHeuristic::print_statistics() const {
    int num_orders = num_best_order.size();
    int num_probably_superfluous = count(num_best_order.begin(), num_best_order.end(), 0);
    int num_probably_useful = num_orders - num_probably_superfluous;
    cout << "Number of times each order was the best order: "
         << num_best_order << endl;
    cout << "Probably useful orders: " << num_probably_useful << "/" << num_orders
         << " = " << 100. * num_probably_useful / num_orders << "%" << endl;
    cout << "Time for computing heuristic: " << *compute_heuristic_timer << endl;
    cout << "Time for converting state: " << *convert_global_state_timer << endl;
    cout << "Time for computing abstract state IDs: " << *get_abstract_state_ids_timer << endl;
    cout << "Time for checking unsolvability: " << *unsolvability_heuristic_timer << endl;
    cout << "Time for computing max_h: " << *compute_max_h_timer << endl;
}
}
