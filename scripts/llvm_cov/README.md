# Coverage with LLVM

Integrates a LLVM Coverage Profdata Merger and Report Generator into `bazel coverage`.

Mimics the normal bazel coverage look and feel by creating a zipped report in the final `_coverage_report.dat` containing a HTML report and a text based source annotation report.

## Setup from a third-party workspace.

Check the [.bazelrc](../../test/.bazelrc) file in the test workspace.

Important notes:

- Check the "Mandatory Coverage options for LLVM" inside the example [.bazelrc](../../test/.bazelrc) which must be used.

## Command

When configured correctly, a normal `bazel coverage` command works:

```bash
bazel coverage //your_package/... --config=your_config
```

For this repo, `bazel coverage //... --config=llvm` works.

To show intermediate steps, use `test-output=all`.
To re-show target specific steps, use `nocache_test_results` which always re-runs tests and therefore the coverage generation.

## Options

### HTML flat view

- `--test_env=HTML_FLAT_VIEW=1`: (0 or 1): If the environment variable `HTML_FLAT_VIEW` is set, then the HTML report will be a flat view, in other words, every file in a single page. Else, the directory view will be used by default.

**Note:** Major LLVM versions older then 19 do not support directory view, so this option must be set when using those.

### Show instantiations

- `--test_env=SHOW_INSTANTIATIONS=1`: (0 or 1): If the environment variable `SHOW_INSTANTIATIONS` is set, then the HTML and textual reports will show each instantiation separately. Otherwise, individual instantiations will be hidden and only the combined summary will be shown.

### Show expansions

- `--test_env=SHOW_EXPANSIONS=1`: (0 or 1): If the environment variable `SHOW_EXPANSIONS` is set, then the HTML and textual reports will expand inclusions, such as preprocessor macros or textual inclusions, inline in the display of the source file. Otherwise, expansions will be hidden.

### Show MCDC

- `--test_env=SHOW_MCDC=1`: (0 or 1): If the environment variable `SHOW_MCDC` is set, then the HTML and textual reports will show MCDC information as a summary and also, inline in the display of the source file. Otherwise, MCDC information will be hidden.

Note: for LLVM-Cov to output MCDC information it is also necessary to properly set up the toolchain. Usually just adding `--copt=-fcoverage-mcdc` to your respective Bazel configuration is already enough, but it can change according to the toolchain.

### Export JSON

- `--test_env=EXPORT_JSON=1`: (0 or 1): If the environment variable `EXPORT_JSON` is set, then a JSON report will be created. Otherwise, no JSON report will be found inside the output zip file.

### Inclusions

- `--test_env=LLVM_COV_INCLUDE=<regex_or_file>`: (String or File): If a string is provided, all files which match the given regex are included in the coverage report. Defaults to `.*`, which matches all files. If a file is provided, every line is treated as a file path to be included in the coverage report.

Note: If a regex is provided, the regex is matched against file paths _relative_ to the workspace root.
That means, if the absolute path of a file is `/workspace/project/foo.cpp`, internally it is seen by bazel as `project/foo.cpp`.
The regex `.*/project/foo.cpp` would not match the file seen by bazel, cause a leading `/` is not part of the internal path.

Examples:

- `--test_env=LLVM_COV_INCLUDE=".*/llvm_cov/package/.*cpp"` would show only `.cpp` files inside the subtree `llvm_cov/package`.
- `--test_env=LLVM_COV_INCLUDE="/path/to/includes.txt"` would show only those files listed in the `/path/to/includes.txt` file.

### Exclusions

- `--test_env=LLVM_COV_EXCLUDE=<regex_or_file>`: (String or File): If a string is provided, all files which match the given regex are excluded in the coverage report. It has no default, so nothing is excluded when the argument is omitted. If a file is provided, every line is treated as a file path to be excluded from the coverage report.

  Examples:

  - `--test_env=LLVM_COV_EXCLUDE=".*/test/.*"` would exclude all files inside any directory called `test`.
  - `--test_env=LLVM_COV_INCLUDE="/path/to/excludes.txt"` would exclude those files listed in the `/path/to/excludes.txt` file.

**NOTE:** If both options are provided, the exclusion pattern "wins". That means, a file which is requested to be included and excluded at the same time, will be excluded.

## Outputs

The runner outputs a zip file for every target + a merged overall zip.

### Per executed Target

Every test target creates a `coverage.dat` file which is a zip file containing:

- HTML report in `html_report`
- Textual report in `text_report`
- Meta info meta.json in `meta`
- Target profdata files in `profdata`

The location is printed at the end of the bazel execution.
It can also be found with `find $(bazel info bazel-testlogs) -name "coverage.dat"`.

For one of this repo's examples a report is located in `$(bazel info bazel-testlogs)/quality_tools/llvm_cov/examples/test/coverage.dat`.

One can use `xdg-open` to open a file on Linux and browser through the zip w/o unpacking it.

### For all executed Targets

All reports are further merged into a overall report located in `bazel-out/_coverage/_coverage_report.dat`. It contains:

- Merged HTML report in `html_report`
- Merged Textual report in `text_report`

## Viewing the Results

A one-command-does-it all:

```bash
unzip -o $(bazel info output_path)/_coverage/_coverage_report.dat -d /tmp/coverage && xdg-open /tmp/coverage/html_report/index.html
```

To browse the zip, also `xdg-open $(bazel info output_path)/_coverage/_coverage_report.dat` works.

## Output Example

```bash
/proc/self/cwd/quality_tools/llvm_cov/examples/package/source.cpp:
    1|       |namespace package
    2|       |{
    3|       |    int Function()
    4|      2|    {
    5|      2|        return 1;
    6|      2|    }
    7|       |}

/proc/self/cwd/quality_tools/llvm_cov/examples/package/test.cpp:
    1|       |#include "quality_tools/llvm_cov/examples/package/source.h"
    2|       |
    3|       |int main()
    4|      2|{
    5|      2|    package::Function();
    6|      2|    return 0;
    7|      2|}

/proc/self/cwd/quality_tools/llvm_cov/examples/source.cpp:
    1|       |int Function()
    2|      2|{
    3|      2|    return 1;
    4|      2|}
    5|       |
    6|       |void AnotherFunction()
    7|      0|{
    8|      0|}

/proc/self/cwd/quality_tools/llvm_cov/examples/source.h:
    1|       |#pragma once
    2|       |
    3|       |int Function();
    4|       |
    5|       |template <typename T>
    6|       |T Template(T t)
    7|      2|{
    8|      2|    if (t > 0)
  ------------------
  |  Branch (8:9): [True: 0, False: 1]
  |  Branch (8:9): [True: 1, False: 0]
  ------------------
    9|      1|        return t * t;
   10|      1|    else
   11|      1|        return t;
   12|      2|}
  ------------------
  | int Template<int>(int):
  |    7|      1|{
  |    8|      1|    if (t > 0)
  |  ------------------
  |  |  Branch (8:9): [True: 0, False: 1]
  |  ------------------
  |    9|      0|        return t * t;
  |   10|      1|    else
  |   11|      1|        return t;
  |   12|      1|}
  ------------------
  | double Template<double>(double):
  |    7|      1|{
  |    8|      1|    if (t > 0)
  |  ------------------
  |  |  Branch (8:9): [True: 1, False: 0]
  |  ------------------
  |    9|      1|        return t * t;
  |   10|      0|    else
  |   11|      0|        return t;
  |   12|      1|}
  ------------------

/proc/self/cwd/quality_tools/llvm_cov/examples/test.cpp:
    1|       |#include "quality_tools/llvm_cov/examples/source.h"
    2|       |#include "quality_tools/llvm_cov/examples/package/source.h"
    3|       |
    4|       |int main()
    5|      2|{
    6|      2|    Function();
    7|      2|    package::Function();
    8|       |
    9|      2|    Function();
   10|       |
   11|      2|    Template(-1);
   12|      2|    Template(1.0);
   13|       |
   14|      2|    return 0;
   15|      2|}

```
