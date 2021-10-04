#include "cost_partitioning_heuristic_collection_generator.h"

#include "cost_partitioning_heuristic.h"
#include "diversifier.h"
#include "order_generator.h"
#include "order_optimizer.h"
#include "utils.h"

#include "../task_proxy.h"

#include "../task_utils/sampling.h"
#include "../task_utils/task_properties.h"
#include "../utils/collections.h"
#include "../utils/countdown_timer.h"
#include "../utils/logging.h"
#include "../utils/memory.h"
#include "../utils/rng.h"

#include <cassert>

using namespace std;

namespace cost_saturation {
static vector<vector<int>> sample_states_and_return_abstract_state_ids(
    const TaskProxy &task_proxy,
    const Abstractions &abstractions,
    sampling::RandomWalkSampler &sampler,
    int num_samples,
    int init_h,
    const DeadEndDetector &is_dead_end,
    double max_sampling_time) {
    assert(num_samples >= 1);
    utils::CountdownTimer sampling_timer(max_sampling_time);
    utils::Log() << "Start sampling" << endl;
    vector<vector<int>> abstract_state_ids_by_sample;
    abstract_state_ids_by_sample.push_back(
        get_abstract_state_ids(abstractions, task_proxy.get_initial_state()));
    while (static_cast<int>(abstract_state_ids_by_sample.size()) < num_samples
           && !sampling_timer.is_expired()) {
        abstract_state_ids_by_sample.push_back(
            get_abstract_state_ids(abstractions, sampler.sample_state(init_h, is_dead_end)));
    }
    utils::Log() << "Samples: " << abstract_state_ids_by_sample.size() << endl;
    utils::Log() << "Sampling time: " << sampling_timer.get_elapsed_time() << endl;
    return abstract_state_ids_by_sample;
}


CostPartitioningHeuristicCollectionGenerator::CostPartitioningHeuristicCollectionGenerator(
    const shared_ptr<OrderGenerator> &order_generator,
    int max_orders,
    int max_size_kb,
    double max_time,
    bool diversify,
    int num_samples,
    double max_optimization_time,
    const shared_ptr<utils::RandomNumberGenerator> &rng)
    : order_generator(order_generator),
      max_orders(max_orders),
      max_size_kb(max_size_kb),
      max_time(max_time),
      diversify(diversify),
      num_samples(num_samples),
      max_optimization_time(max_optimization_time),
      rng(rng) {
}

vector<CostPartitioningHeuristic>
CostPartitioningHeuristicCollectionGenerator::generate_cost_partitionings(
    const TaskProxy &task_proxy,
    const Abstractions &abstractions,
    const vector<int> &costs,
    const CPFunction &cp_function) const {
    utils::Log log;
    utils::CountdownTimer timer(max_time);

    State initial_state = task_proxy.get_initial_state();

    order_generator->initialize(abstractions, costs);

    vector<int> abstract_state_ids_for_init = get_abstract_state_ids(
        abstractions, initial_state);
    Order order_for_init = order_generator->compute_order_for_state(
        abstract_state_ids_for_init, true);
    vector<int> remaining_costs = costs;
    CostPartitioningHeuristic cp_for_init = cp_function(
        abstractions, order_for_init, remaining_costs, abstract_state_ids_for_init);
    int init_h = cp_for_init.compute_heuristic(abstract_state_ids_for_init);

    if (init_h == INF) {
        log << "Initial state is unsolvable." << endl;
        return {
                   cp_for_init
        };
    }

    sampling::RandomWalkSampler sampler(task_proxy, *rng);
    DeadEndDetector is_dead_end =
        [&abstractions, &cp_for_init](const State &state) {
            return cp_for_init.compute_heuristic(
                get_abstract_state_ids(abstractions, state)) == INF;
        };

    unique_ptr<Diversifier> diversifier;
    if (diversify) {
        double max_sampling_time = timer.get_remaining_time();
        diversifier = utils::make_unique_ptr<Diversifier>(
            sample_states_and_return_abstract_state_ids(
                task_proxy, abstractions, sampler, num_samples, init_h, is_dead_end, max_sampling_time));
    }

    log << "Start computing cost partitionings" << endl;
    vector<CostPartitioningHeuristic> cp_heuristics;
    int evaluated_orders = 0;
    int size_kb = 0;

    /* Loop over systematic projection orders, create full orders and add them
       if they're diverse or unconditionally, if diversification is off. */
    for (const Order &sys_order : systematic_generator_orders_hacked) {
        if (timer.is_expired() && !cp_heuristics.empty()) {
            break;
        }

        Order order = sys_order;
        unordered_set<int> used(order.begin(), order.end());
        vector<int> abstraction_ids = get_default_order(abstractions.size());
        rng->shuffle(abstraction_ids);
        for (int abs_id : abstraction_ids) {
            if (used.insert(abs_id).second) {
                order.push_back(abs_id);
            }
        }
        vector<int> remaining_costs = costs;
        CostPartitioningHeuristic cp_heuristic = cp_function(
            abstractions, order, remaining_costs, abstract_state_ids_for_init);
        if (!diversifier || diversifier->is_diverse(cp_heuristic)) {
            cp_heuristics.push_back(move(cp_heuristic));
            if (diversifier) {
                log << "Average finite h value for " << num_samples
                    << " samples after " << timer.get_elapsed_time()
                    << " of diversification for systematic sequences: "
                    << diversifier->compute_avg_finite_sample_h_value()
                    << endl;
            }
        }
    }
    int num_projection_orders = systematic_generator_orders_hacked.size();
    int num_selected_projection_orders = cp_heuristics.size();
    double selected_percentage = (num_projection_orders == 0) ? 0
        : num_selected_projection_orders / static_cast<double>(num_projection_orders);
    log << "Selected projection orders: " << num_selected_projection_orders
        << "/" << num_projection_orders << " = " << selected_percentage << endl;

    while (static_cast<int>(cp_heuristics.size()) < max_orders &&
           (!timer.is_expired() || cp_heuristics.empty()) &&
           (size_kb < max_size_kb)) {
        bool is_first_order = (evaluated_orders == 0);

        vector<int> abstract_state_ids;
        Order order;
        CostPartitioningHeuristic cp_heuristic;
        if (is_first_order) {
            // Use initial state as first sample.
            abstract_state_ids = abstract_state_ids_for_init;
            order = order_for_init;
            cp_heuristic = cp_for_init;
        } else {
            abstract_state_ids = get_abstract_state_ids(
                abstractions, sampler.sample_state(init_h, is_dead_end));
            order = order_generator->compute_order_for_state(
                abstract_state_ids, false);
            vector<int> remaining_costs = costs;
            cp_heuristic = cp_function(abstractions, order, remaining_costs, abstract_state_ids);
        }

        // Optimize order.
        double optimization_time = min(
            static_cast<double>(timer.get_remaining_time()), max_optimization_time);
        if (optimization_time > 0) {
            utils::CountdownTimer opt_timer(optimization_time);
            int incumbent_h_value = cp_heuristic.compute_heuristic(abstract_state_ids);
            optimize_order_with_hill_climbing(
                cp_function, opt_timer, abstractions, costs, abstract_state_ids, order,
                cp_heuristic, incumbent_h_value, is_first_order);
            if (is_first_order) {
                log << "Time for optimizing order: " << opt_timer.get_elapsed_time()
                    << endl;
            }
        }

        // If diversify=true, only add order if it improves upon previously
        // added orders.
        if (!diversifier || diversifier->is_diverse(cp_heuristic)) {
            size_kb += cp_heuristic.estimate_size_in_kb();
            cp_heuristics.push_back(move(cp_heuristic));
            if (diversifier) {
                log << "Average finite h-value for " << num_samples
                    << " samples after " << timer.get_elapsed_time()
                    << " of diversification: "
                    << diversifier->compute_avg_finite_sample_h_value()
                    << endl;
            }
        }

        ++evaluated_orders;
    }

    log << "Evaluated orders: " << evaluated_orders << endl;
    log << "Cost partitionings: " << cp_heuristics.size() << endl;
    log << "Time for computing cost partitionings: " << timer.get_elapsed_time()
        << endl;
    log << "Estimated heuristic size: " << size_kb << " KiB" << endl;
    return cp_heuristics;
}
}
