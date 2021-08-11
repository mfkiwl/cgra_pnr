#include "timing.hh"
#include "thunder_io.hh"
#include <unordered_set>

#include <queue>

using Netlist = std::vector<Net>;

std::vector<std::pair<int, const Pin *>> get_source_pins(const std::vector<Net> &netlist) {
    std::vector<std::pair<int, const Pin *>> result;
    // any pin that has I as the name is an IO pin
    for (auto const &net: netlist) {
        auto const &p = net[0];
        if (p.name[0] == 'i' || p.name[0] == 'I') {
            result.emplace_back(std::make_pair(net.id, &p));
        }
    }
    return result;
}

// simple graph to topological sort and figure out the timing
struct TimingNode {
    std::string name;
    std::vector<const Pin *> src_pins;
    std::vector<const Pin *> sink_pins;
    std::vector<TimingNode *> next;
};

class TimingGraph {
public:
    explicit TimingGraph(const Netlist &netlist) : netlist_(netlist) {
        for (auto const &net: netlist) {
            auto const &src_pin = net[0];
            auto *src_node = get_node(src_pin);
            src_node->sink_pins.emplace_back(&src_pin);
            for (uint64_t i = 1; i < net.size(); i++) {
                auto const &sink = net[i];
                auto *sink_node = get_node(sink);
                src_node->next.emplace_back(sink_node);
                sink_node->src_pins.emplace_back(&sink);
            }
        }
    }

    std::vector<const TimingNode *> topological_sort() const {
        std::vector<const TimingNode *> result;
        std::unordered_set<const TimingNode *> visited;

        for (auto const &node: nodes_) {
            if (visited.find(node.get()) == visited.end()) {
                sort_(result, visited, node.get());
            }
        }

        std::reverse(result.begin(), result.end());
        return result;
    }

    std::vector<int> get_sink_ids(const TimingNode *node) const {
        std::vector<int> result;
        for (auto const *n: node->next) {
            for (auto const &net: netlist_) {
                if (net[0].name == node->name) {
                    result.emplace_back(net.id);
                }
            }
        }

        return result;
    }

private:
    const Netlist &netlist_;
    std::unordered_map<std::string, TimingNode *> name_to_node_;
    std::unordered_set<std::unique_ptr<TimingNode>> nodes_;

    TimingNode *get_node(const Pin &pin) {
        auto const &name = pin.name;
        if (name_to_node_.find(name) == name_to_node_.end()) {
            auto node_ptr = std::make_unique<TimingNode>();
            auto *ptr = node_ptr.get();
            nodes_.emplace(std::move(node_ptr));
            name_to_node_.emplace(name, ptr);
        }
        return name_to_node_.at(name);
    }

    void sort_(std::vector<const TimingNode *> &result, std::unordered_set<const TimingNode *> &visited,
               const TimingNode *node) const {
        visited.emplace(node);
        for (auto const *n: node->next) {
            if (visited.find(n) == visited.end()) {
                sort_(result, visited, n);
            }
        }
        result.emplace_back(node);
    }

};


std::unordered_set<const Pin *> get_sink_pins(const Pin &pin, const Netlist &netlist) {
    // brute-force search
    std::unordered_set<const Pin *> result;
    for (auto const &net: netlist) {
        auto const &src = net[0];
        if (src.x == pin.x && src.y == pin.y && src.name[0] != 'r') {
            // it's placed on the same tile, but it's not a pipeline register
            result.emplace(&src);
        }
    }

    return result;
}

std::unordered_map<const Node *, const TimingNode *>
get_timing_node_mapping(const std::vector<const TimingNode *> &nodes) {
    std::unordered_map<const Node *, const TimingNode *> result;
    for (auto const *node: nodes) {
        for (auto const *src: node->src_pins) {
            result.emplace(src->node.get(), node);
        }
        for (auto const *sink: node->sink_pins) {
            result.emplace(sink->node.get(), node);
        }
    }

    return result;
}


void TimingAnalysis::retime() {
    auto const &netlist = router_.get_netlist();
    auto const routed_graphs = router_.get_routed_graph();

    auto io_pins = get_source_pins(netlist);
    auto allowed_delay = maximum_delay();

    std::unordered_map<const Pin *, uint64_t> pin_delay_;
    std::unordered_map<const TimingNode *, uint64_t> node_delay_;
    std::unordered_map<const Pin *, uint64_t> pin_wave_;
    std::unordered_map<const TimingNode *, uint64_t> node_wave_;

    for (auto const &[id, pin]: io_pins) {
        pin_wave_.emplace(pin, 0);
    }

    const TimingGraph timing_graph(netlist);

    auto nodes = timing_graph.topological_sort();
    auto timing_node_mapping = get_timing_node_mapping(nodes);
    std::map<std::string, std::vector<std::vector<std::shared_ptr<Node>>>> final_result;

    // start STA on each node
    for (auto const *timing_node: nodes) {
        // the delay table is already calculated after the input, i.e., we don't consider the src pin
        // delay
        uint64_t start_delay = node_delay_[timing_node];
        auto sink_net_ids = timing_graph.get_sink_ids(timing_node);
        for (auto const net_id: sink_net_ids) {
            auto const &net = netlist[net_id];
            auto const &routed_graph = routed_graphs.at(net.id);

            // all its source pins have to be available
            auto const &src_pins = timing_node->sink_pins;
            uint64_t max_delay = 0;
            std::unordered_set<uint64_t> pin_waves;
            for (auto const *src_pin: src_pins) {
                if (pin_wave_.find(src_pin) == pin_wave_.end()) {
                    throw std::runtime_error("Unable to find wave number for " + src_pin->name);
                }
                if (pin_delay_.find(src_pin) == pin_delay_.end()) {
                    throw std::runtime_error("Unable to find pin delay for " + src_pin->name);
                }
                pin_waves.emplace(pin_wave_.at(src_pin));
                if (pin_delay_.at(src_pin) > max_delay) {
                    max_delay = pin_delay_.at(src_pin);
                }
            }
            // we assume at this point the pin data waves should be matched
            if (pin_wave_.size() != 1) {
                throw std::runtime_error("Node pins data wave does not match: " + timing_node->name);
            }

            // now we need to compute the delay for each node
            std::unordered_map<const Node *, uint64_t> node_delay = {{net[0].node.get(), max_delay}};
            auto segments = routed_graph.get_route();
            for (auto const &segment: segments) {
                for (uint64_t i = 1; i < segment.size(); i++) {
                    auto const &current_node = segment[i];
                    auto const &pre_node = segment[i - 1];
                    if (node_delay.find(pre_node.get()) == node_delay.end()) {
                        throw std::runtime_error("Unable to find delay for node " + pre_node->name);
                    }
                    auto delay = node_delay.at(pre_node.get());
                    delay += get_delay(current_node.get());

                    // if the delay is more than we can handle, we need to insert the pipeline registers
                    if (delay > allowed_delay) {
                        // need to pipeline register it
                        
                    }
                }
            }

            // get the updated route
            segments = routed_graph.get_route();
            final_result.emplace(net.name, segments);
        }
    }
}

void TimingAnalysis::set_layout(const std::string &path) {
    layout_ = load_layout(path);
}

uint64_t TimingAnalysis::get_delay(const Node *node) {
    switch (node->type) {
        case NodeType::Port: {
            auto const &node_name = node->name;
            auto type = node_name[0];
            switch (type) {
                case 'p':
                    return timing_cost_.at(TimingCost::CLB_OP);
                case 'm':
                    // assume memory is registered
                    return timing_cost_.at(TimingCost::MEM);
                default:
                    throw std::runtime_error("Unable to identify delay for node: " + node->name);
            }
        }
        case NodeType::Register: {
            return timing_cost_.at(TimingCost::REG);
        }
        case NodeType::SwitchBox: {
            // need to determine if it's input or output, and the location
            auto *sb = reinterpret_cast<const SwitchBoxNode *>(node);
            if (sb->io == SwitchBoxIO::SB_IN) {
                return 0;
            } else {
                // need to figure out the tile type
                auto clb_type = layout_.get_blk_type(node->x, node->y);
                switch (clb_type) {
                    case 'p':
                        return timing_cost_.at(TimingCost::CLB_SB);
                    case 'm':
                        return timing_cost_.at(TimingCost::MEM_SB);
                    case 'i':
                        return 0;
                    default:
                        throw std::runtime_error("Unable to identify timing for blk " + node->name);
                }
            }
        }
        case NodeType::Generic: {
            return timing_cost_.at(TimingCost::RMUX);
        }
    }
}

uint64_t TimingAnalysis::maximum_delay() const {
    // the frequency is in mhz
    auto ns = 1'000'000 / min_frequency_;
    return ns;
}
