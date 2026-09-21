# MOPK

## Clone

```bash
git clone --branch expand-generated-data-range --single-branch https://github.com/asardinka/MOPK.git
```

## Data format

Each input CSV contains:

```text
id,string,data,float64_1,float64_2,float64_3,float64_4,float64_5,float64_6,float64_7
```

Rows with the same `string + date` are merged, and all seven `float64` fields are summed independently.

After changing the generator or pulling this version, regenerate the datasets:

```bash
g++ -std=c++17 make_csv.cpp -o build/make_csv.exe && build/make_csv.exe
```

## Build and run

Run from the repository root:

```bash
g++ -std=c++17 -O2 test14.cpp -o build/test14.exe && build/test14.exe
```

For comparison of optimization strategies:

```bash
g++ -std=c++17 -O2 bigTest.cpp -o build/bigTest.exe && build/bigTest.exe
```

Datasets are stored in `data/`. Executable files are stored in `build/`.
