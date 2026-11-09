/**
TODO: Clone-Funktionalität
TODO: Cadical-Optionen weitergeben
TODO: maybe dont use the terminator if performance is bad. also explore the cadical options for it

TODO: -v  increase verbose level
TODO: -q  do not print 'c job ...' lines (requires verbosity < 2)
TODO: -b  progress bar (implies '-q')
TODO: -s  <stats> output statistics to separate file
TODO: -t  <hist> output job run time histogram to separate file
TODO:  --clone       use cloning for hard cubes
TODO:  --reduce      reduce learned clause cache after each job
TODO:  --no-melt     do not melt unused assumption variables
TODO:  --no-flush    do not flush learned clause cache before every job
TODO:  --no-reverse  do not reverse assumptions
TODO:  --deterministic | --det
            jobs are mapped deterministically to workers
TODO:  --no-add    do not add failed assumptions as don't care
TODO:  -A  add all assumptions as don't care
TODO:  <inccnf>    'p inccnf' + '<lit*> 0' clauses + 'a <lit>* 0' assumptions
TODO:  <nworkers>  number of workers defaults to 1
TODO:  -d|--drup   <path-prefix-for-traces>


TODO:

*/

#include <cassert>
#include <thread>
#include <mutex>
#include <iostream>
#include <string>
#include <vector>
#include "cadical.hpp"
#include "resources.hpp"
#include "terminal.hpp"

struct Worker : CaDiCaL::Terminator {
    const int id; // für Logging
    int last; // letzter Job, den der Worker bearbeitet hat -> beim Bearbeiten der Aktuelle
    int result; // Ergebnis: SAT, UNSAT, UNKNOWN
    double startTime;
    double startTimeProcess;
    double endTime;
    double endTimeProcess;
    double realTime;
    double realTimeProcess;
    FILE *proof, *post;
    int *failed, numFailed;
    CaDiCaL::Solver * solver;
    Worker (int i) : id (i), last(-1), solver (new CaDiCaL::Solver) { }
    ~Worker() {
        solver->disconnect_terminator();
        delete solver;
    }
    // cadical has Terminator-Interface. We connect this Terminator in initialization
    // the solver checks this method "regularly" and if it returns true terminates by itself
    // this means we don't need to send some kill signal to Cadical but just implement this
    bool terminate();
    void initWorker(Worker * mainWorker) {
        mainWorker->solver->copy(*(this->solver));
        this->solver->connect_terminator(this);
        this->msg(1, "worker initialized");
        startTime = CaDiCaL::absolute_real_time();
        startTimeProcess = CaDiCaL::absolute_process_time();
    }
    void stopWorker() {
        endTime = CaDiCaL::absolute_real_time();
        endTimeProcess = CaDiCaL::absolute_process_time();
        realTime = startTime - endTime;
        realTimeProcess = startTimeProcess - endTimeProcess;
    }
    void work();
    void msg(int level, const char * fmt...) const;
};

static FILE *statsFile, *histFile;
static double cumulatedTime = 0;
static double cumulatedProcessTime = 0;

static const int verbosity = 1;
static int queue = 0;
static std::mutex queueMutex;
static int numCubes;
static int satCubes;
static int unsatCubes;
static int inconclusiveCubes;

static bool satFound = false;
static std::mutex doneMutex;
static std::mutex msgMutex;

std::vector<std::vector<int>> cubes;
static Worker * winningWorker;

static void __msg(const Worker * worker, int level, const char *fmt, va_list ap) {
    if (verbosity < level) return;
    const std::lock_guard<std::mutex> msgLock(msgMutex);
    if (worker) {
        std::printf("c %d ", worker->id);
    } else {
        std::printf("c - ");
    }
    std::vprintf(fmt, ap);
    std::printf("\n");
} // releaseMsgLock

void _msg(int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __msg(0, level, fmt, ap);
    va_end(ap);
}

void section(int level, const char * title) {
    _msg(level, "%s--- [ %s ] ---------------------------------------------------------%s", CaDiCaL::tout.blue_code(), title, CaDiCaL::tout.normal_code());
}

void Worker::msg(int level, const char *fmt, ...) const {
    va_list ap;
    va_start(ap, fmt);
    __msg(this, level, fmt, ap);
    va_end(ap);
}

bool Worker::terminate()  {
    // msg(1, "checking early termination");
    const std::lock_guard<std::mutex> doneLock(doneMutex);
    // msg(1, "early termination check %s", satFound ? "succeeded" : "failed");
    return satFound;
}

void readCNF(const Worker * worker, const char * filePath) {
    //      const char *read_dimacs (const char *path, int &vars, int strict,
    //                           bool &incremental, std::vector<int> &cubes);

    // worker->solver->read_dimacs(filePath);
    bool isIncremental; // iCNF-Header gefunden?
    int vars; // größte Variablen-ID
    std::vector<int> assumptions;

    // error if =! 0
    // strict = 1 (auch bei Cadical Standard) TODO: mehr Optionen ermöglichen
    const char * err = worker->solver->read_dimacs(filePath, vars, 1, isIncremental, assumptions);
    if (err) {
        worker->msg(1, "err: parsing failed: %s", err);
    }

    // cubes are just a flat array and Cadical doesnt return the num so we have to go through it again
    int lastElem = 0;
    for (auto elem : assumptions) {
        if (!lastElem) cubes.emplace_back(std::vector<int>()); // start with new cube
        if (!elem) {
            numCubes++;
            lastElem = 0;
        } else {
            cubes.at(numCubes).emplace_back((lastElem = elem));
        }
    }
}

void Worker::work() {
    int jobId;
    for (;;) {
        {
            const std::lock_guard<std::mutex> doneLock(doneMutex);
            if (satFound) break;
        }
        {
            const std::lock_guard<std::mutex> queueLock(queueMutex);
            if ((jobId = queue) >= numCubes) {
                msg(1, "worker has no available jobs. stopping the worker");
                break;
            } else {
                queue++;
            }
        } // release queueLock
        assert(jobId < numCubes);
        msg(1, "got job %d", jobId);
        auto currentAssumptions = cubes.at(jobId);
        // set new assumptions
        for (auto elem : currentAssumptions) {
            solver->assume(elem);
            msg(2, "assume %d", elem);
        }
        double solveStart = CaDiCaL::absolute_process_time();
        result = solver->solve();
        double solveDelta = CaDiCaL::absolute_process_time() - solveStart;
        msg(1, "finished with solving job %d with result: %d", jobId, result);
        // we don't have to reset as the Cadical solve()-method does it already
        // solver->reset_assumptions();
        if (result == CaDiCaL::Status::SATISFIABLE) {
            msg(0, "%sCUBE %d SATISFIABLE%s in %.3f sec %s", CaDiCaL::tout.green_code(), jobId, CaDiCaL::tout.magenta_code(), solveDelta, CaDiCaL::tout.normal_code());
            const std::lock_guard<std::mutex> doneLock(doneMutex);
            winningWorker = this;
            satFound = true;
            break;
        } else if (result == CaDiCaL::Status::UNSATISFIABLE) {
            msg(0, "%sCUBE %d UNSATISFIABLE%s in %.3f sec %s", CaDiCaL::tout.cyan_code(), jobId, CaDiCaL::tout.magenta_code(), solveDelta, CaDiCaL::tout.normal_code());
        } else {
            msg(0, "%sCUBE %d inconclusive%s in %.3f sec %s", CaDiCaL::tout.magenta_code(), jobId, CaDiCaL::tout.magenta_code(), solveDelta, CaDiCaL::tout.normal_code());
        }
    }
    msg(1, "worker has finished");
}

int main(int argc, char ** argv) {
    // First we create the thread pool
    // Default number of workers is just the amount of logical processors we have
    // TODO: add cli parameter to set it by hand
    int threadCount = std::thread::hardware_concurrency();
    threadCount = 128;
    Worker mainWorker(-1);
    readCNF(&mainWorker, "../sat-benchmarks/icnf/sgen1-sat-160-100.icnf");
    _msg(1, "%d cubes parsed", numCubes);
    double time = CaDiCaL::absolute_real_time();
    _msg(0, "time %f", time);

    std::vector<std::thread> threads;
    for (int i = 0; i < threadCount; i++) {
        threads.emplace_back([i, &mainWorker] {
            Worker worker(i);
            // workers.emplace_back(worker);
            worker.initWorker(&mainWorker);
            worker.work();
        });
    }
    for (auto & t : threads) {
        t.join();
    }
    section(0, "incremental summary");

    section(0, "result");
    if (satFound) {
        std::printf("s SATISFIABLE\n");
        // _msg(0, "s SATISFIABLE");
    } else {
        std::printf("s UNSATISFIABLE\n");
        // _msg(0, "s UNSATISFIABLE");
    }

    // TODO: correct finding of "winner"
    if (winningWorker && winningWorker->result == CaDiCaL::Status::SATISFIABLE) {
        // we have a winner!
        // printf("vars: %d", winningWorker->solver->vars());
        // winningWorker->solver->statistics();
        // winningWorker->solver->resources();
        // CaDiCaL::Solver::build(stdout);
    }


    _msg(0, "time %f", CaDiCaL::absolute_real_time() - time);
}
