/**
TODO: Clone-Funktionalität

TODO: -s  <stats> output statistics to separate file
TODO:  --clone       use cloning for hard cubes
TODO:  --reduce      reduce learned clause cache after each job
TODO:  -d|--drup   <path-prefix-for-traces>
*/
// #define NDEBUG
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
    iCadicalOption<int> progressBar{
        "progress-bar", "shows a progress bar and silences all output", 0};
    // dont add the assumptions in reverse order
    iCadicalOption<int> noReverse{
        "no-reverse", "dont add the assumptions in reverse order", 0};
    // --no-flush -> do not flush learned clause cache before every job
    // by default the learned clause cache is flushed every time before a new
    // job
    iCadicalOption<int> noFlush{
        "no-flush", "do not flush learned clause cache before every job", 0};
    // do not add failed assumptions as don't care
    iCadicalOption<int> noAssumptions{"no-assumptions",
                                      "don't add assumptions", 0};
    // add all assumptions as don't care
    iCadicalOption<int> allAssumptions{"all-assumptions",
                                       "add all assumptions", 0};
    iCadicalOption<int> freezeCubes{"freeze", "use freezing", 0};
    // do not melt unused assumption variables
    iCadicalOption<int> noMelt{"no-melt",
                               "do not melt unused assumption variables", 0};
    // each job gets assigned an id from [0, numWorkers]
    // each worker only takes jobs with the corresponding id
    iCadicalOption<int> deterministicScheduling{
        "deterministic",
        "deterministically assign jobs to workers based on their id", 0};
    // by default is set to the amount of cpu cores available
    iCadicalOption<int> numWorkers{
        "num-workers <amount>", "amount of workers to handle the cubes",
        std::thread::hardware_concurrency()
            ? static_cast<int>(std::thread::hardware_concurrency())
            : 1};
    // set strict parsing to one of cadicals options (0, 1 or 2)
    iCadicalOption<int> strictParsing{"strict <0,1,2>",
                                      "strict parsing enabled", 1};

    iCadicalOption<int> optimization{"optimization", "optimization limits", 0};
    iCadicalOption<int> conflictLimit{
        "conflict-limit", "limit the number of conflicts in each solver", -1};
    iCadicalOption<int> decisionLimit{
        "decision-limit", "limit the number of decisions in each solver", -1};

    iCadicalOption<std::string> icnfPath{"<input.icnf>",
                                         "path of the icnf file", "stdin"};
    iCadicalOption<std::string> proofPath{"<proof>", "path of the proof file",
                                          "no output"};
    iCadicalOption<std::string> statsPath{
        "--stats-file <path>", "output statistics to separate file", "stdout"};
    iCadicalOption<std::string> histPath{
        "--hist-file <path>", "output job run time histogram to separate file",
        "stdout"};

    std::vector<iCadicalOption<std::string> *> stringOptions;
    std::vector<iCadicalOption<int> *> intOptions;
    // also pass explicitly given options to CaDiCaL
    std::vector<std::string> cadicalOptions;
    // cadical can also take default configuration
    std::vector<std::string> cadicalConfigurations;

    Options() {
        stringOptions = {&icnfPath, &proofPath, &statsPath, &histPath};
        intOptions = {
            &verbosity,      &progressBar,  &noReverse,
            &noFlush,        &freezeCubes,  &conflictLimit,
            &decisionLimit,  &quiet,        &noAssumptions,
            &allAssumptions, &noMelt,       &deterministicScheduling,
            &numWorkers,     &optimization, &strictParsing,
        };
    }
};
static Options options;

struct Worker : CaDiCaL::Terminator {
    const int id;    // für Logging
    int lastJob{-1}; // letzter Job, den der Worker bearbeitet hat -> beim
                     // Bearbeiten der Aktuelle
    int lastSeenCube{-1};
    int result{CaDiCaL::Status::UNKNOWN}; // Ergebnis: SAT, UNSAT, UNKNOWN
    double startTime;
    double startTimeProcess;
    double endTime;
    double endTimeProcess;
    double realTime;
    double realTimeProcess;
    FILE *proof, *post;
    int *failed, numFailed;
    std::unique_ptr<CaDiCaL::Solver> solver;
    Worker() = delete; // MUST always have an id so forbid default constructor
    Worker(const int i) : id(i), solver(new CaDiCaL::Solver) {}
    ~Worker() {
        // with unique_ptr the solver is deleted automatically after we
        // destruct our Worker
        solver->disconnect_terminator();
    }
    // cadical has Terminator-Interface. We connect this Terminator in
    // initialization the solver checks this method "regularly" and if it
    // returns true terminates by itself this means we don't need to send some
    // kill signal to Cadical but just implement this
    bool terminate() override;
    void initWorker(Worker * mainWorker) {
        mainWorker->solver->copy(*this->solver);
        this->solver->connect_terminator(this);
        this->msg(1, "worker initialized");
        startTime = CaDiCaL::absolute_real_time();
        startTimeProcess = CaDiCaL::absolute_process_time();
    }
    void stopWorker() {
        endTime = CaDiCaL::absolute_real_time();
        endTimeProcess = CaDiCaL::absolute_process_time();
        realTime = endTime - startTime;
        realTimeProcess = endTimeProcess - startTimeProcess;
    }
    void work();
    void msg(int level, const char * fmt...) const;
};

static double totalStartTime;
static double totalStartTimeProcess;
static std::vector<double> cubeTimes;

static int numCubes;
static std::atomic<int> satCubes(0);
static std::atomic<int> unsatCubes(0);
static std::atomic<int> inconclusiveCubes(0);
static std::atomic<int> solvedCubes(0);

static std::atomic<bool> satFound(false);
static std::atomic<int> queue(0);
static std::mutex msgMutex;
static std::mutex assumptionMutex;

static std::vector<std::vector<int>> cubes;
static std::vector<std::vector<int>> failedAssumptions;
static std::vector<Worker *> workers;
static Worker * winningWorker;

FILE * histFile;
FILE * statsFile;

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

static void __msg(const Worker * worker, int level, const char * fmt,
                  va_list ap) {
    if (options.verbosity.get() < level) {
        return;
    }
    const std::lock_guard<std::mutex> msgLock(msgMutex);
    if (worker) {
        std::printf("c %d ", worker->id);
    } else {
        std::printf("c ");
    }
    std::vprintf(fmt, ap);
    std::printf("\n");
} // releaseMsgLock

static void _msg(const int level, const char * fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __msg(nullptr, level, fmt, ap);
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

void Worker::msg(int level, const char * fmt, ...) const {
    if (options.quiet.get()) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    __msg(this, level, fmt, ap);
    va_end(ap);
}

// TODO: maybe in extra thread and run only each 500ms or so
static void showProgress(bool finish = false) {
    //  don't divide by 0
    int solvCubes = solvedCubes.load();
    if (!numCubes || !solvCubes) {
        return;
    }
    double timeDelta = CaDiCaL::absolute_real_time() - totalStartTime;
    int percentSolved = (solvCubes * 100) / numCubes;
    int startDashes = percentSolved / 5;
    int endDashes = 20 - startDashes;
    const std::lock_guard<std::mutex> msgLock(msgMutex);
    fputc('\r', stdout);
    std::printf("c %*d / %d [%s%c%s] %02d%% (%.4f sec/cube)",
                snprintf(nullptr, 0, "%d", numCubes), solvCubes, numCubes,
                std::string(startDashes, '=').c_str(),
                ('<' + ((solvCubes / 100) % 3)),
                std::string(endDashes, '-').c_str(), percentSolved,
                timeDelta / solvCubes);
    std::fflush(stdout);
    if (finish) {
        std::printf("\n");
    }
}

bool Worker::terminate() {
    msg(2, "early termination check on worker %d: %s", id,
        satFound.load() ? "succeeded" : "failed");
    return satFound.load(std::memory_order_relaxed);
}

static bool readCNF(const Worker * worker, const char * filePath) {
    bool isIncremental; // iCNF-Header gefunden?
    int vars;           // größte Variablen-ID
    std::vector<int> assumptions;

    // returns error with message if parsing failed. else just null
    const char * err = worker->solver->read_dimacs(filePath, vars,
                                                   options.strictParsing.get(),
                                                   isIncremental, assumptions);
    if (err) {
        worker->msg(0, "err: parsing failed: %s", err);
        return false;
    }

    // cubes are just a flat array and Cadical doesnt return the num so we have
    // to go through it again
    int lastElem = 0;
    for (auto elem : assumptions) {
        if (!lastElem) {
            cubes.emplace_back(std::vector<int>()); // start with new cube
        }
        lastElem = elem;
        if (!elem) {
            numCubes++;
            lastElem = 0;
        } else {
            cubes[numCubes].emplace_back((lastElem = elem));
        }
    }
    assumptions.clear();
    return true;
}

void Worker::work() {
    int jobId = -1;
    while (true) {
        if (satFound.load()) {
            break;
        }
        if (options.deterministicScheduling.get()) {
            if (jobId == -1) {
                jobId = id;
            } else {
                jobId = jobId + options.numWorkers.get();
            }
        } else {
            jobId = queue.fetch_add(1);
        }
        if (jobId >= numCubes) {
            msg(1, "worker has no available jobs. stopping the worker");
            break;
        }
        assert(jobId != lastJob);
        assert(jobId < numCubes);
        msg(1, "got job %d", jobId);

        // replay all the assumptions
        // TODO: check deterministic mode with this!
        if (options.allAssumptions.get()) {
            // std::vector<int>
            std::lock_guard<std::mutex> assumptionGuard(assumptionMutex);
            for (auto it = failedAssumptions.begin() + lastSeenCube + 1;
                 it != failedAssumptions.end(); ++it) {
                assert(!it->empty());
                solver->clause(*it);
            }
            lastSeenCube = static_cast<int>(failedAssumptions.size()) - 1;
        }

        if (options.freezeCubes.get() && !options.noMelt.get()) {
            std::lock_guard<std::mutex> assumptionGuard(assumptionMutex);
            for (auto it = failedAssumptions.begin() + lastJob + 1;
                 it != failedAssumptions.begin() + jobId; ++it) {
                if (!it->empty()) {
                    for (int lit : *it) {
                        // if (used abs(lit)) {}
                        if (solver->frozen(abs(lit))) {
                            solver->melt(
                                abs(lit)); // stored as -lit so reverse
                        }
                    }
                }
            }
        }

        // no guard needed as only read in the threads
        auto const & currentAssumptions = cubes[jobId];
        // reverse the assumptions
        // looks like it is faster, but ... why?
        if (options.noReverse.get()) {
            for (auto it = currentAssumptions.begin();
                 it < currentAssumptions.end(); ++it) {
                solver->assume(*it);
                msg(2, "assume %d", *it);
            }
        } else {
            for (auto it = currentAssumptions.rbegin();
                 it < currentAssumptions.rend(); ++it) {
                solver->assume(*it);
                msg(2, "assume %d", *it);
            }
        }
        if (options.conflictLimit.get()) {
            solver->limit("conflicts", options.conflictLimit.get());
        }
        if (options.decisionLimit.get()) {
            solver->limit("decisions", options.decisionLimit.get());
        }

        double solveStart = CaDiCaL::absolute_real_time();
        result = solver->solve();
        double solveDelta = CaDiCaL::absolute_real_time() - solveStart;
        cubeTimes[jobId] = solveDelta;
        lastJob = jobId;
        msg(1, "finished with solving job %d with result: %d", lastJob,
            result);
        int percent = (jobId * 100) / numCubes; // !numCubes guarded in main()
        double passedTime = CaDiCaL::absolute_real_time() - startTime;
        double cubeTime = !jobId ? solveDelta : (passedTime * 10) / jobId;
        if (result != CaDiCaL::Status::UNKNOWN) {
            solvedCubes.fetch_add(1);
        }
        if (result == CaDiCaL::Status::SATISFIABLE) {
            if (!options.progressBar.get()) {
                msg(0,
                    "%sCUBE %d SATISFIABLE%s in %.4f sec (%d%% after %.2f sec "
                    "at avg "
                    "%.4f sec/cube))%s",
                    CaDiCaL::tout.green_code(), lastJob,
                    CaDiCaL::tout.magenta_code(), solveDelta, percent,
                    passedTime, cubeTime, CaDiCaL::tout.normal_code());
            }
            satFound.store(true);
            satCubes.fetch_add(1);
            solver->conclude();
            break;
        } else if (result == CaDiCaL::Status::UNSATISFIABLE) {
            if (!options.progressBar.get()) {
                msg(0,
                    "%sCUBE %d UNSATISFIABLE%s in %.4f sec (%d%% after %.2f "
                    "sec at "
                    "avg %.4f sec/cube)%s",
                    CaDiCaL::tout.cyan_code(), lastJob,
                    CaDiCaL::tout.magenta_code(), solveDelta, percent,
                    passedTime, cubeTime, CaDiCaL::tout.normal_code());
            }
            unsatCubes.fetch_add(1);
            solver->conclude();
        } else {
            if (!options.progressBar.get()) {
                msg(0,
                    "%sCUBE %d UNCONCLUSIVE%s in %.4f sec (%d%% after %.2f "
                    "sec at avg "
                    "%.4f sec/cube)%s",
                    CaDiCaL::tout.bright_magenta_code(), lastJob,
                    CaDiCaL::tout.magenta_code(), solveDelta, percent,
                    passedTime, cubeTime, CaDiCaL::tout.normal_code());
            }
            inconclusiveCubes.fetch_add(1);
        }
        if (options.progressBar.get()) {
            showProgress();
        }

        // TODO: what does this do?
        // solver.cpp:1328 (calls conclude_sat, ...)
        // assume.cpp:461: conclude_unsat:
        // passes it to the proof tracer
        // TODO: if proof tracing is wanted this is needed probably
        // solver->conclude();

        // find out the failed core
        // the failed core is added as a 'learned clause' to the solver
        // will be preserved by copy() as the irredundant clauses are copied
        // if it is redundant then we dont care anyways
        // must be done before copy() to find out the failed literals
        if (!options.noAssumptions.get()) {
            if (result == CaDiCaL::Status::UNSATISFIABLE) {
                // assert(solver->state() == CaDiCaL::State::UNSATISFIED);
                std::vector<int> _failedAssumptions;
                // In Cadical the assumptions get reversed (cf.
                // cadical.cpp:773) here we only change the view while
                // iterating so it has to be reversed also here or else we get
                // differing results from Cadical
                if (options.noReverse.get()) {
                    for (auto it = currentAssumptions.begin();
                         it != currentAssumptions.end(); ++it) {
                        if (solver->failed(*it)) {
                            msg(1, "FAILED %d ADDING %d", *it, -*it);
                            _failedAssumptions.push_back(-*it);
                            // failedAssumptions.push_back(lit);
                            // solver->add(-lit); (changes the solver state
                            // ...)
                        }
                    }
                } else {
                    for (auto it = currentAssumptions.rbegin();
                         it != currentAssumptions.rend(); ++it) {
                        if (solver->failed(*it)) {
                            msg(1, "FAILED %d ADDING %d", *it, -*it);
                            _failedAssumptions.push_back(-*it);
                            // failedAssumptions.push_back(lit);
                            // solver->add(-lit); (changes the solver state
                            // ...)
                        }
                    }
                }
                if (!_failedAssumptions.empty()) {
                    // will be replayed in the beginning if allAssumptions is
                    // set if not it might be difficult to find it again so
                    // just add the clause already here
                    if (!options.allAssumptions.get()) {
                        solver->clause(_failedAssumptions);
                    } else {
                        std::lock_guard<std::mutex> assumptionGuard(
                            assumptionMutex);
                        failedAssumptions.emplace_back(_failedAssumptions);
                    }
                }
            }
        }

        // deleting the whole solver state brings considerable speedup
        // there is no option in Cadical to delete the learned clauses
        // so we have to copy it
        // redundant (i.e. learned cf) clauses get
        // accounts for basically all of our runtime besides solving
        if (!options.noFlush.get()) {
            std::unique_ptr<CaDiCaL::Solver> newSolver(new CaDiCaL::Solver);
            solver->copy(*newSolver);
            solver = std::move(newSolver);
            // we've thrown away the terminator so get it back
            solver->connect_terminator(this);
            // int siRes = solver->simplify();
            // assert(!siRes);
        }
    }
    stopWorker();
    msg(1, "worker has finished");
}

int main(int argc, char ** argv) {
    Worker mainWorker(-1);
    // default number of workers is just the amount of logical processors we
    // have but might also return 0
    options.numWorkers.def =
        static_cast<int>(std::thread::hardware_concurrency());
    options.numWorkers.set(options.numWorkers.def);
    if (!options.numWorkers.def) {
        options.numWorkers.set(1);
        options.numWorkers.def = 1;
    }

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
        } else if (arg == "--num-workers") {
            // we have to check the next arg
            if (argc <= ++i) {
                error("parse error: num-workers needs a numeric amount of "
                      "workers "
                      "supplied");
                return 1;
            } else {
                int val;
                if (!CaDiCaL::parse_int_str(args[i].c_str(), val)) {
                    error("parse error: num-workers needs an int argument "
                          "(supplied: %s)",
                          args[i].c_str());
                    return 1;
                }
                if (val < 1) {
                    error("parse error: num-workers needs to be at least 1 "
                          "(supplied: %s)",
                          args[i].c_str());
                    return 1;
                }
                options.numWorkers.set(val);
            }
        } else if (arg == "--progress-bar") {
            options.progressBar.set(1);
        } else if (arg == "--no-flush") {
            options.noFlush.set(1);
        } else if (arg == "--no-reverse") {
            options.noReverse.set(1);
        } else if (arg == "--no-assumptions") {
            options.noAssumptions.set(1);
        } else if (arg == "--all-assumptions") {
            options.allAssumptions.set(1);
        } else if (arg == "--freeze") {
            options.freezeCubes.set(1);
        } else if (arg == "--no-melt") {
            options.noMelt.set(1);
        } else if (arg == "--deterministic") {
            options.deterministicScheduling.set(1);
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
        } else if (arg == "--decision-limit" || arg == "-d") {
            // we have to check the next arg
            if (argc <= ++i) {
                error("parse error: decision-limit needs a number supplied");
                return 1;
            } else {
                int val;
                if (!CaDiCaL::parse_int_str(args[i].c_str(), val)) {
                    error("parse error: decision-limit needs an int argument "
                          "(supplied: "
                          "%s)",
                          args[i].c_str());
                    return 1;
                }
                options.decisionLimit.set(val);
            }
        } else if (arg == "--conflict-limit" || arg == "-c") {
            // we have to check the next arg
            if (argc <= ++i) {
                error("parse error: decision-limit needs a number supplied");
                return 1;
            } else {
                int val;
                if (!CaDiCaL::parse_int_str(args[i].c_str(), val)) {
                    error("parse error: decision-limit needs an int argument "
                          "(supplied: "
                          "%s)",
                          args[i].c_str());
                    return 1;
                }
                options.conflictLimit.set(val);
            }
        } else if (arg == "--hist-file") {
            if (argc <= ++i) {
                error("parse error: hist-file needs a file path supplied");
                return 1;
            } else {
                if ((histFile = fopen(args[i].c_str(), "w"))) {
                    options.histPath.set(args[i]);
                } else {
                    error("parse error: histogram file could not be opened "
                          "(supplied "
                          "path: %s)",
                          args[i].c_str());
                    return 1;
                }
            }
        } else if (arg == "--stats-file") {
            if (argc <= ++i) {
                error("parse error: stats-file needs a file path supplied");
                return 1;
            } else {
                if ((statsFile = fopen(args[i].c_str(), "w"))) {
                    options.statsPath.set(args[i]);
                } else {
                    error("parse error: stats file could not be opened "
                          "(supplied path: %s)",
                          args[i].c_str());
                    return 1;
                }
            }
        } else if (arg == "--strict") {
            if (argc <= ++i) {
                error("parse error: strict needs a number supplied");
                return 1;
            } else {
                int val;
                if (!CaDiCaL::parse_int_str(args[i].c_str(), val)) {
                    error("parse error: strict needs an int argument "
                          "(supplied: %s)",
                          args[i].c_str());
                    return 1;
                }
                if (val < 0 || val > 2) {
                    error("parse error: strict needs to be 0, 1 or 2 "
                          "(supplied: %d)",
                          val);
                    return 1;
                } else {
                    options.strictParsing.set(val);
                }
            }
        }
        // all options which are unknown are assummed to be passed to cadical
        else {
            if (mainWorker.solver->is_valid_long_option(arg.c_str())) {
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
        options.icnfPath.set(positionalPaths[0]);
    }
    if (positionalPaths.size() > 1) {
        options.proofPath.set(positionalPaths[1]);
    }

    if (options.allAssumptions.get() && options.noAssumptions.get()) {
        error("parse error: --all-assumptions and --no-assumptions can't be "
              "used "
              "together");
        return 1;
    }

    if (options.noMelt.get() && !options.freezeCubes.get()) {
        error("parse error: --no-melt is only respected when used with "
              "--freeze");
        return 1;
    }

    for (auto & elem : options.cadicalOptions) {
        mainWorker.solver->set_long_option(elem.c_str());
    }

    section(0, "banner", false);

    mainWorker.solver->build(stdout);
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
            mainWorker.solver->set_long_option(option.c_str());
            std::printf("c  %s\n", option.c_str());
        }
    }

    section(0, "parsing input");

    if (!readCNF(&mainWorker, options.icnfPath.get().c_str())) {
        error("couldnt read file %s. stopping!",
              options.icnfPath.get().c_str());
        return 1;
    }

    if (options.optimization.get() > 0) {
        mainWorker.solver->optimize(options.optimization.get());
    }

    // mainWorker.solver->simplify();

    if (!numCubes) {
        std::printf("c no cubes could be parsed. continuing to solve with a "
                    "single worker\n");
        options.numWorkers.set(0);
        mainWorker.result = mainWorker.solver->solve();
        winningWorker = &mainWorker;
    } else {
        // initialize global vectors
        cubeTimes.assign(numCubes, -1.0);
        // reserve the space so that our pushes are faster
        failedAssumptions.reserve(numCubes);
        // failedAssumptions.assign(numCubes, std::vector<int>());

        section(0, "incremental solving");
        _msg(0, "starting to solve %d cubes with %d workers\nc", numCubes,
             options.numWorkers.get());
        for (int i = 0; i < options.numWorkers.get(); i++) {
            workers.push_back(new Worker(i));
            workers[i]->initWorker(&mainWorker);
            int highestVar = mainWorker.solver->vars();
            if (options.freezeCubes.get()) {
                for (int j = 1; j < highestVar; j++) {
                    workers[i]->solver->freeze(j);
                }
            }
        }

        std::vector<std::thread> threads;
        threads.reserve(options.numWorkers.get());
        totalStartTime = CaDiCaL::absolute_real_time();
        for (int i = 0; i < options.numWorkers.get(); i++) {
            threads.emplace_back([i] {
                // workers[i]->initWorker(&mainWorker);
                workers[i]->work();
            });
        }
        for (auto & t : threads) {
            t.join();
        }

        if (options.progressBar.get()) {
            showProgress(true);
        }

        section(0, "incremental summary");

        // TODO: does inconclusive count as solved?
        if (!numCubes) {
            _msg(0, "no cubes in the file");
        } else {
            _msg(0, "%d cubes solved %d%%",
                 satCubes.load() + unsatCubes.load(),
                 ((satCubes + unsatCubes) * 100) / numCubes);
            _msg(0, "%d cubes inconclusive %d%%", inconclusiveCubes.load(),
                 (inconclusiveCubes * 100) / numCubes);
            _msg(0, "%d cubes unsatisfiable %d%%", unsatCubes.load(),
                 (unsatCubes * 100) / numCubes);
            _msg(0, "%d cubes satisfiable %d%%", satCubes.load(),
                 (satCubes * 100) / numCubes);
        }

        // find the "winning worker"
        // in case of a SAT worker this is the first worker with result SAT
        // in case of a UNSAT/UNKNOWN worker this is the first worker result
        // we can only report UNSAT if all cubes were UNSAT
        for (auto & worker : workers) {
            if (!winningWorker) {
                winningWorker = worker;
                _msg(1, "worker %d is the winning worker", winningWorker->id);
                continue;
            }
            if (worker->result == CaDiCaL::Status::SATISFIABLE) {
                winningWorker = worker;
                break;
            }
        }
    }

    section(0, "result");

    if (satCubes.load() > 0) {
        // we have a winner! now print the result
        std::printf("s SATISFIABLE\n");
        int maxVar = winningWorker->solver->vars();
        // shamelessly steal print_witness() function from cadical
        int lineLength = 0; // ignore 'v' in the beginning
        for (int lit = 1; lit <= maxVar; lit++) {
            if (!lineLength) {
                std::printf("v");
            }
            int val = winningWorker->solver->val(lit);
            if (!val) {
                continue;
            }
            int len = snprintf(nullptr, 0, " %d",
                               val); // still returns the 'would be' length
            if (lineLength + len > 80) {
                std::printf("\nv");
                lineLength = 0;
            }
            lineLength += len;
            std::printf(" %d", val);
        }
        std::printf(" 0\nc\n");
    } else if (numCubes == unsatCubes.load()) {
        std::printf("s UNSATISFIABLE\nc\n");
    } else {
        std::printf("s UNKNOWN\nc\n");
    }

    // section(0, "statistics");
    winningWorker->solver->statistics();

    section(0, "resources");
    _msg(0, "total process time since initialization: %.2f\tseconds",
         CaDiCaL::absolute_process_time() - totalStartTimeProcess);
    _msg(0, "total real time since initialization: %.2f\tseconds",
         CaDiCaL::absolute_real_time() - totalStartTime);

    if (options.histPath.get() != options.histPath.def) {
        for (auto time : cubeTimes) {
            fprintf(histFile, "%.4f\n", time);
        }
        fflush(histFile);
        fclose(histFile);
    }

    section(0, "shutting down");
    _msg(0, "%d\nc", winningWorker->result);
    return winningWorker->result;
    // exit(winningWorker->result);
}
