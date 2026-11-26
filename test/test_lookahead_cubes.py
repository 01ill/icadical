# small helper script to check if the cubes actually differ

lit_combinations = []

with open("factoring63.icnf") as f:
    content = f.readlines()
    filtered = [line for line in content if line.startswith("a")]

for cube in filtered:
    lits = " ".join([lit.replace("-", "") for lit in cube.split(" ")])
    if lits not in lit_combinations:
        lit_combinations.append(lits)

print(lit_combinations)
