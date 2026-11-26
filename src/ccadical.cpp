// #define NDEBUG
#include <string>
#include <random>
#include <fstream>
#include <vector>
#include <cassert>

#include "cadical/cadical.hpp"
#include "cadical/resources.hpp"
#include "cadical/terminal.hpp"
#include "cadical/util.hpp"

template <typename OptionType> struct iCadicalOption {
    const char * name;
    const char * description;
    OptionType def;
    OptionType val;
    iCadicalOption(const char * name, const char * description, OptionType def)
        : name(name), description(description), def(def), val(def) {}
    void set(OptionType value) { val = std::move(value); }
    OptionType get() { return val; }
};

struct Options {
    iCadicalOption<int> quiet{"quiet",
                              "do not print messages from the workers", 0};
    iCadicalOption<int> verbosity{"verbosity", "sets the verbose level", 0};
    iCadicalOption<std::string> cnfPath{"<input.cnf>", "path of the input cnf file",
                                          "stdin"};
    iCadicalOption<std::string> icnfPath{"<output.icnf>",
                                         "path of the output icnf file", "stdout"};
    iCadicalOption<int> depth{"depth", "specify maximum amount of split variables, i.e. cube length", 10};
    std::vector<iCadicalOption<std::string> *> stringOptions;
    std::vector<iCadicalOption<int> *> intOptions;
    // also pass explicitly given options to CaDiCaL
    std::vector<std::string> cadicalOptions;
    // cadical can also take default configuration
    std::vector<std::string> cadicalConfigurations;

    Options() {
        stringOptions = {&icnfPath, &cnfPath};
        intOptions = {
            &verbosity,  &quiet, &depth
        };
    }
};
static Options options;

static void printUsage() {
    std::printf(
        "usage: icadical [ <option> ...] [ <input.icnf> [ <proof> ] ]\n"
        "\n"
        "where you can pass the following paths:\n");
    for (auto option : options.stringOptions) {
        std::printf("  %-27s%-60s [%s]\n", option->name, option->description,
                    option->def.c_str());
    }
    std::printf("\nwhere '<option>' is one of the following options:\n");
    for (auto option : options.intOptions) {
        std::printf("  --%-25s%-60s [%d]\n", option->name, option->description,
                    option->def);
    }

    std::printf("\nall other <options> are passed to CaDiCaL\n");
}

static void __msg(int level, const char * fmt, va_list ap) {
    if (options.verbosity.get() < level) {
        return;
    }
    std::printf("c ");
    std::vprintf(fmt, ap);
    std::printf("\n");
} // releaseMsgLock

static void _msg(const int level, const char * fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __msg(level, fmt, ap);
    va_end(ap);
}

static void verror(const char * fmt, va_list & ap) {
    std::fprintf(stderr, "%sicadical: %serror:%s ", CaDiCaL::terr.bold_code(),
                 CaDiCaL::terr.bright_red_code(), CaDiCaL::terr.normal_code());
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
}

static void error(const char * fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    verror(fmt, ap);
    va_end(ap);
}

static void section(int level, const char * title, bool lineBefore = true) {
    int len = std::snprintf(nullptr, 0, " --- [ %s ] ", title);
    int dashCount = 80 - len;
    if (lineBefore) {
        _msg(level, "");
    }
    _msg(level, "%s--- [ %s%s%s%s ] %s%s\nc", CaDiCaL::tout.blue_code(),
         CaDiCaL::tout.bold_code(), title, CaDiCaL::tout.normal_code(),
         CaDiCaL::tout.blue_code(), std::string(dashCount, '-').c_str(),
         CaDiCaL::tout.normal_code());
}

static bool readCNF(CaDiCaL::Solver * solver, const char * filePath) {
    bool isIncremental; // iCNF-Header gefunden?
    int vars;           // größte Variablen-ID
    std::vector<int> assumptions;

    // returns error with message if parsing failed. else just null
    const char * err =
        solver->read_dimacs(filePath, vars, 1,
                            isIncremental, assumptions);
    if (err) {
        _msg(0, "err: parsing failed: %s", err);
        return false;
    }
    return true;
}

// for now a small helper to generate the cubes
// later on might be helpful for concurrent lookahead/solving
struct TreeNode {
    int parent = -1;
    int left{-1}; // left = assign true
    int right{-1}; // right = assign false
    // the assumptions +- are given to the children
    // the root does lookahead but cant make +-. so zero
    int lit = 0;
};

struct DecisionTree {
    std::vector<TreeNode> nodes;

    DecisionTree() {
        nodes.emplace_back(TreeNode());
    }

    std::vector<int> val_walk(int node_idx) {
        std::vector<int> chain{};
        TreeNode node = nodes[node_idx];
        while (node.parent != 0) {
            chain.push_back(node.lit);
            node = nodes[node.parent];
        }
        return chain;
    }

    void print_tree() {
        for (int i = 0; i < static_cast<int>(this->nodes.size()); i++) {
            std::printf("node %d: lit: %d, parent: %d, left: %d, right: %d\n", i, this->nodes[i].lit, this->nodes[i].parent, this->nodes[i].left, this->nodes[i].right);
        }
    }
};


static int build_decision_tree_dynamic(CaDiCaL::Solver * solver, std::vector<std::vector<int>> &cubes, int max_depth = 10, float rand_factor = 0.5) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> distr(0, 1);
    std::vector<int> assumptions{};
    DecisionTree dt = DecisionTree();
    int depth = 0;
    int node_idx = 0;
    bool backtrack = false;
    // cant get any longer
    std::vector<int> chain(max_depth, 0);
    while (true) {
        TreeNode parent_node = dt.nodes[node_idx];
        if (backtrack) {
            std::printf("backtrack at depth %d\n", depth);
            std::printf("node: lit: %d, parent: %d, left: %d, right: %d\n", parent_node.lit, parent_node.parent, parent_node.left, parent_node.right);

            // we've hit a stop so we must reset all assumptions
            // (cadical provides no granular control over assumptions)
            solver->reset_assumptions();
            // leaf nodes also have -1 in their childs so backtrack at least once
            do {
                depth--;
                node_idx = dt.nodes[node_idx].parent;

            } while (dt.nodes[node_idx].right != -1 && node_idx != 0);
            parent_node = dt.nodes[node_idx];
            std::printf("backtrack finished at depth %d. backtracked to:\n", depth);
            std::printf("node: lit: %d, parent: %d, left: %d, right: %d\n", parent_node.lit, parent_node.parent, parent_node.left, parent_node.right);
            backtrack = false;

            if (node_idx == 0 && parent_node.right != -1 && parent_node.left != -1) {
                break;
            }
        }

        // reached max depth: set cutoff leaf and backtrack
        // max_depth "=" levels, i.e. stops after depth -1
        if (depth >= max_depth) {
            std::printf("reached max depth %d. GENERATED CUBE: ", depth);
            assumptions.clear();
            for (int i = 0; i < depth; i++) {
                std::printf("%d ", chain[i]);
                assumptions.push_back(chain[i]);
            }
            cubes.push_back(assumptions);
            std::printf("\n");
            backtrack = true;
            continue;
        }


        // we dont have any child nodes so create new ones and
        if (parent_node.left == -1) {
            int splitter = solver->lookahead();
            std::printf("lookahead: %d\n", splitter);

            // splitter solved the formula: might be SAT, UNSAT or inconclusive when empty formula
            // if SAT we can end it all
            // if UNSAT we can refute the leaf
            // else ??? (for now assert that it doesnt happen ...)
            if (splitter == 0) {
                if (solver->state() == CaDiCaL::State::SATISFIED) {
                    return CaDiCaL::Status::SATISFIABLE;
                } else if (solver->state() == CaDiCaL::State::UNSATISFIED) {
                    depth = 0;
                    node_idx = 0;
                    backtrack = true;
                    continue;
                }
                assert(solver->state() != CaDiCaL::State::INCONCLUSIVE);
            }

            TreeNode left = TreeNode();
            left.parent = node_idx;
            left.lit = splitter;
            dt.nodes.emplace_back(left);
            dt.nodes[node_idx].left = static_cast<int>(dt.nodes.size() - 1);
            node_idx = static_cast<int>(dt.nodes.size() - 1);

            for (int i = 0; i < depth; i++) {
                solver->assume(chain[i]);
            }
            solver->assume(splitter);
            std::printf("assuming %d at depth %d\n", splitter, depth);
            chain[depth] = splitter;
        } else if (parent_node.right == -1) {
            int value = dt.nodes[parent_node.left].lit;
            TreeNode right = TreeNode();
            right.parent = node_idx;
            right.lit = -value;
            dt.nodes.emplace_back(right);
            dt.nodes[node_idx].right = static_cast<int>(dt.nodes.size() - 1);
            node_idx = static_cast<int>(dt.nodes.size() - 1);

            for (int i = 0; i < depth; i++) {
                solver->assume(chain[i]);
            }
            solver->assume(-value);
            std::printf("assuming %d at depth %d\n", -value, depth);
            chain[depth] = -value;
        }
        depth++;
    }
    std::printf("finished building decision tree\n");
    dt.print_tree();
    return 0;
}

static int build_decision_tree_static(CaDiCaL::Solver * solver, std::vector<std::vector<int>> &cubes, int max_depth = 10, float rand_factor = 0.5) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> distr(0, 1);
    std::vector<int> assumptions{};
    DecisionTree dt = DecisionTree();
    int depth = 0;
    int node_idx = 0;
    bool backtrack = false;
    // cant get any longer
    std::vector<int> chain(max_depth, 0);
    while (true) {
        TreeNode parent_node = dt.nodes[node_idx];
        if (backtrack) {
            std::printf("backtrack at depth %d\n", depth);
            std::printf("node: lit: %d, parent: %d, left: %d, right: %d\n", parent_node.lit, parent_node.parent, parent_node.left, parent_node.right);

            // we've hit a stop so we must reset all assumptions
            // (cadical provides no granular control over assumptions)
            solver->reset_assumptions();
            // leaf nodes also have -1 in their childs so backtrack at least once
            do {
                depth--;
                node_idx = dt.nodes[node_idx].parent;

            } while (dt.nodes[node_idx].right != -1 && node_idx != 0);
            parent_node = dt.nodes[node_idx];
            std::printf("backtrack finished at depth %d. backtracked to:\n", depth);
            std::printf("node: lit: %d, parent: %d, left: %d, right: %d\n", parent_node.lit, parent_node.parent, parent_node.left, parent_node.right);
            backtrack = false;

            if (node_idx == 0 && parent_node.right != -1 && parent_node.left != -1) {
                break;
            }
        }

        // reached max depth: set cutoff leaf and backtrack
        // max_depth "=" levels, i.e. stops after depth -1
        if (depth >= max_depth) {
            std::printf("reached max depth %d. GENERATED CUBE: ", depth);
            assumptions.clear();
            for (int i = 0; i < depth; i++) {
                std::printf("%d ", chain[i]);
                assumptions.push_back(chain[i]);
            }
            cubes.push_back(assumptions);
            std::printf("\n");
            backtrack = true;
            continue;
        }


        // we dont have any child nodes so create new ones and
        if (parent_node.left == -1) {
            int splitter = solver->lookahead();
            std::printf("lookahead: %d\n", splitter);

            // splitter solved the formula: might be SAT, UNSAT or inconclusive when empty formula
            // if SAT we can end it all
            // if UNSAT we can refute the leaf
            // else ??? (for now assert that it doesnt happen ...)
            if (splitter == 0) {
                if (solver->state() == CaDiCaL::State::SATISFIED) {
                    return CaDiCaL::Status::SATISFIABLE;
                } else if (solver->state() == CaDiCaL::State::UNSATISFIED) {
                    depth = 0;
                    node_idx = 0;
                    backtrack = true;
                    continue;
                }
                assert(solver->state() != CaDiCaL::State::INCONCLUSIVE);
            }

            TreeNode left = TreeNode();
            left.parent = node_idx;
            left.lit = splitter;
            dt.nodes.emplace_back(left);
            dt.nodes[node_idx].left = static_cast<int>(dt.nodes.size() - 1);
            node_idx = static_cast<int>(dt.nodes.size() - 1);

            for (int i = 0; i < depth; i++) {
                solver->assume(chain[i]);
            }
            solver->assume(splitter);
            std::printf("assuming %d at depth %d\n", splitter, depth);
            chain[depth] = splitter;
        } else if (parent_node.right == -1) {
            int value = dt.nodes[parent_node.left].lit;
            TreeNode right = TreeNode();
            right.parent = node_idx;
            right.lit = -value;
            dt.nodes.emplace_back(right);
            dt.nodes[node_idx].right = static_cast<int>(dt.nodes.size() - 1);
            node_idx = static_cast<int>(dt.nodes.size() - 1);

            for (int i = 0; i < depth; i++) {
                solver->assume(chain[i]);
            }
            solver->assume(-value);
            std::printf("assuming %d at depth %d\n", -value, depth);
            chain[depth] = -value;
        }
        depth++;
    }
    std::printf("finished building decision tree\n");
    dt.print_tree();
    return 0;
}


static bool write_icnf(const char *in_path, const char *out_path, const std::vector<std::vector<int>> &cubes) {
    std::ifstream in(in_path);
    std::ofstream out(out_path);

    if (!in || !out) return false;

    std::string line;
    // copy the file and rewrite the header
    while (std::getline(in, line)) {
        if (line.rfind("p cnf", 0) == 0) {
            out << "p inccnf\n";
        } else {
            out << line << "\n";
        }
    }

    for (const auto & cube : cubes) {
        out << 'a';
        for (int lit : cube) {
            if (lit) out << ' ' << lit;
        }
        out << " 0\n";
    }
    return static_cast<bool>(out);
}

static void localsearch_test(CaDiCaL::Solver * solver) {
    // solver->set_long_option("--quiet=0");
    // solver->set_long_option("--verbose=3");
    solver->limit("localsearch", 50);
    solver->limit("conflicts", 0);
    int res = solver->solve();
    std::printf("res: %d\n", res);
    std::printf("%d\n", solver->saved_phase(1));
    std::printf("%d\n", solver->saved_phase(2));
    std::vector<int> most_flips = solver->localsearch_most_flipped(10);
    for (auto flip : most_flips) {
        std::printf("count: %d\n", flip);
    }
    std::printf("\n");

    // solver->statistics();
}

static int
localsearch_static_tree(CaDiCaL::Solver * solver,
                        std::vector<std::vector<int>> & cubes, int depth,
                        int rounds) {
    solver->limit("localsearch", rounds);
    solver->limit("conflicts", 0);
    int res = solver->solve();
    std::vector<int> most_flips = solver->localsearch_most_flipped(depth);
    for (auto flip : most_flips) {
        std::printf("count: %d\n", flip);
    }
    std::printf("\n");
    std::vector<int> assumptions{};
    DecisionTree dt = DecisionTree();
    int node_idx = 0;
    int current_depth = 0;
    bool backtrack = false;
    // cant get any longer
    std::vector<int> chain(depth, 0);
    while (true) {
        TreeNode parent_node = dt.nodes[node_idx];
        if (backtrack) {
            std::printf("backtrack at depth %d\n", current_depth);
            std::printf("node: lit: %d, parent: %d, left: %d, right: %d\n",
                        parent_node.lit, parent_node.parent, parent_node.left,
                        parent_node.right);

            // leaf nodes also have -1 in their childs so backtrack at least
            // once
            do {
                current_depth--;
                node_idx = dt.nodes[node_idx].parent;

            } while (dt.nodes[node_idx].right != -1 && node_idx != 0);
            parent_node = dt.nodes[node_idx];
            std::printf("backtrack finished at depth %d. backtracked to:\n",
                        current_depth);
            std::printf("node: lit: %d, parent: %d, left: %d, right: %d\n",
                        parent_node.lit, parent_node.parent, parent_node.left,
                        parent_node.right);
            backtrack = false;

            if (node_idx == 0 && parent_node.right != -1 &&
                parent_node.left != -1) {
                break;
            }
        }

        // reached max depth: set cutoff leaf and backtrack
        // max_depth "=" levels, i.e. stops after depth -1
        if (current_depth >= depth) {
            std::printf("reached max depth %d. GENERATED CUBE: ",
                        current_depth);
            assumptions.clear();
            for (int i = 0; i < current_depth; i++) {
                std::printf("%d ", chain[i]);
                assumptions.push_back(chain[i]);
            }
            cubes.push_back(assumptions);
            std::printf("\n");
            backtrack = true;
            continue;
        }

        // we dont have any child nodes so create new ones and
        if (parent_node.left == -1) {
            int splitter = most_flips[current_depth];
            std::printf("lookahead: %d\n", splitter);

            TreeNode left = TreeNode();
            left.parent = node_idx;
            left.lit = splitter;
            dt.nodes.emplace_back(left);
            dt.nodes[node_idx].left = static_cast<int>(dt.nodes.size() - 1);
            node_idx = static_cast<int>(dt.nodes.size() - 1);
            chain[current_depth] = splitter;
            std::printf("assuming %d at depth %d\n", splitter, current_depth);
        } else if (parent_node.right == -1) {
            int value = dt.nodes[parent_node.left].lit;
            TreeNode right = TreeNode();
            right.parent = node_idx;
            right.lit = -value;
            dt.nodes.emplace_back(right);
            dt.nodes[node_idx].right = static_cast<int>(dt.nodes.size() - 1);
            node_idx = static_cast<int>(dt.nodes.size() - 1);
            chain[current_depth] = -value;
            std::printf("assuming %d at depth %d\n", -value, current_depth);
        }
        current_depth++;
    }
    std::printf("finished building decision tree\n");
    dt.print_tree();
    return 0;
}

int main(int argc, char ** argv) {
    CaDiCaL::Solver * solver = new CaDiCaL::Solver;
    solver->set_long_option("--verbose=3");
    std::vector<std::string> args(argv, argv + argc);
    std::vector<std::string> positionalPaths;
    int _verbosity = 0;
    for (int i = 1; i < static_cast<int>(args.size()); i++) {
        std::string & arg = args[i];
        // like cadical: if no path is supplied we take the stdin
        // TODO: actually take the stdin then
        if (arg[0] != '-' || args.size() == 1) {
            positionalPaths.emplace_back(arg);
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        } else if (arg == "-v") {
            _verbosity++;
        } else if (arg == "--quiet" || arg == "-q") {
            options.quiet.set(1);
        } else if (arg == "--optimization" || arg == "-O") {
            if (argc <= ++i) {
                error("parse error: optimization needs a number supplied");
                return 1;
            }
            int val;
            if (!CaDiCaL::parse_int_str(args[i].c_str(), val)) {
                error("parse error: decision-limit needs an int argument "
                      "(supplied: %s)",
                      args[i].c_str());
                return 1;
            }
        } else if (arg == "--depth" ) {
            if (argc <= ++i) {
                error("parse error: depth needs a number supplied");
                return 1;
            }
            int val;
            if (!CaDiCaL::parse_int_str(args[i].c_str(), val)) {
                error("parse error: depth needs an int argument "
                      "(supplied: %s)",
                      args[i].c_str());
                return 1;
            }
            options.depth.set(val);
        }
        // all options which are unknown are assummed to be passed to
        // cadical
        else {
            if (solver->is_valid_long_option(arg.c_str())) {
                options.cadicalOptions.emplace_back(arg);
            } else {
                error("parse error: option not recognised by cadical! "
                      "(supplied: %s)",
                      arg.c_str());
                return 1;
            }
        }

    }
    options.verbosity.set(_verbosity);

    if (positionalPaths.size() > 2) {
        error("parse error: too many positional parameters supplied! wont "
              "continue!");
        return 1;
    }
    if (positionalPaths.size() > 0) {
        options.cnfPath.set(positionalPaths[0]);
    }
    if (positionalPaths.size() > 1) {
        options.icnfPath.set(positionalPaths[1]);
    }

    for (auto & elem : options.cadicalOptions) {
        // solver->set_long_option(elem.c_str());
    }


    section(0, "banner", false);

    solver->build(stdout);
    section(0, "parsed arguments", false);
    std::printf("c Using the following settings:\n");
    for (auto option : options.intOptions) {
        if (option->get() != option->def) {
            std::printf("c  %-25s = %d\n", option->name, option->get());
        }
    }
    for (auto option : options.stringOptions) {
        if (option->get() != option->def) {
            std::printf("c  %-25s = %s\n", option->name,
                        option->get().c_str());
        }
    }
    if (!options.cadicalOptions.empty()) {
        std::printf("c Passing the following settings to CaDiCaL:\n");
        for (auto & option : options.cadicalOptions) {
            // TODO: actually set the option...
            solver->set_long_option(option.c_str());
            std::printf("c  %s\n", option.c_str());
        }
    }

    section(0, "parsing input");

    if (!readCNF(solver, options.cnfPath.get().c_str())) {
        error("couldnt read file %s. stopping!",
              options.cnfPath.get().c_str());
        return 1;
    }
    std::vector<std::vector<int>> cubes;
    // localsearch_test(solver);
    localsearch_static_tree(solver, cubes, 10, 50);
    std::printf("generated cubes: %zu\n", cubes.size());
    write_icnf(options.cnfPath.get().c_str(), options.icnfPath.get().c_str(), cubes);

    int result = 0;
    // build_decision_tree_dynamic(solver, cubes, options.depth.get());
    // write_icnf(options.cnfPath.get().c_str(), options.icnfPath.get().c_str(), cubes);
    return result;
}
