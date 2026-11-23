# icadical

icadical is a fronted for the CaDiCaL SAT solver which can solve incremental CNF files in parallel.

## Dependencies
The CaDiCaL library and several headers are used to provide the CaDiCaL backend and import some helper functions.

The needed files are required to be in a subfolder with the cadical repository. Also the `libcadical.a` is required which means that CaDiCaL needs to be built once.
The provided makefile clones the repository and builds CaDiCaL if it is not found.

## Usage

The project is built with `clang++` and a simple makefile. To run it you have to do the following:
```bash
make
./build/icadical <options> <icnf file>
```

To solve the sample icnf file with e.g. 2 threads run the following:
```bash
./build/icadical --num-workers 2 test/prime49.icnf
```

The most important options are explained here:
- `--num-workers <threads>` sets the amount of used threads. By default is the reported amount of available cpus via `std::thread::hardware_concurrency()`
- `--no-flush` disables the reset of the solver instances after each finished cube. By default after each solved cube the solver instance is reset with cadicals `copy()` method to get rid of learned clauses.
- `--no-assumptions`: by default after a cube is UNSAT the cube is added manually as a "learned clause" with its negated literals to the workers own solver. This might be detrimental for some CNFs. This disables this features.
- `--all-assumptions`: this enables the addition of the negated failed cubes to the solvers of all workers. This not only adds the own workers failed cubes but also the negated cubes of the other workers. Can bring a speedup on some CNFs.
- `--<cadical-option>=<value>` passes the option to CaDiCaL.
- `--deterministic` enables deterministic scheduling of jobs to threads. Keep in mind that results might still differ because some workers finish earlier and thus results might not be yet available to other workers.
- `--quiet` disables the per cube reporting. Results in a minimal speedup because the workers don't need to wait on each others output.
- `--progress-bar` enables a progress bar instead of the default output of each solved cube

For the full list of options run:
```bash
./build/icadical --help
```

# ccadical
ccadical is a frontend for cube creation using CaDiCaLs lookahead function.

## Usage
The project is built with `clang++` and a simple makefile. To run it you have to do the following:
```bash
make
./build/ccadical <options>  <cnf file> <icnf file>
```

To generate an icnf with cubes from a sample cnf file with cubing depth of 5 run the following:
```bash
./build/ccadical --depth 5 samples/test.cnf samples/test.icnf
```

Currently the following options exist:
- `--depth <cube depth>` the maximum amount of variables in each cube. might be smaller if the lookahead in Cadical already solves some subformula.
