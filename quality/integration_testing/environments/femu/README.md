# Fuchsia emulator (FEMU) integration tests (experimental)

ITF plugin (`femu`) and `femu_integration_test` macro (in `integration_testing.bzl`) that boot the
Fuchsia emulator headless, publish Fuchsia packages to a temporary repository and run test
components with `ffx test run`.

```python
def test_hello(target):
    result = target.run_test_component("hello_world_test#meta/hello_world_test.cm")
    assert "Hello, World from Fuchsia!" in result.output
```

```bash
bazel test --config=fuchsia //quality/integration_testing/test/femu_hello_world:hello_world_itf_test
```

## Setup

- `rules_fuchsia`, the Fuchsia SDK and clang are not on the BCR. They are fetched from CIPD through
  [`fuchsia_infra`](https://fuchsia.googlesource.com/fuchsia-infra-bazel-rules) (dev dependency with
  `git_override` in the root `MODULE.bazel`). Versions are pinned in `//third_party/fuchsia`
  (`*.ensure` / `*.resolved`).
- The `core.x64` product bundle (about 360 MB) is fetched by `//third_party/fuchsia:product_bundle.bzl`
  from the public Fuchsia artifact bucket. Keep it in sync with the SDK version in
  `bazel_sdk.ensure` (`ffx sdk version`, currently 33.20260919.6.1).
- The Fuchsia toolchains are only registered with `--config=fuchsia` (root `.bazelrc`) and all
  targets are tagged `manual`, so regular builds and `//...` are not affected.

## Sandboxing

The test runs in the Bazel linux-sandbox without network (`--nosandbox_default_allow_network`,
tmpfs `/tmp`). SDK, `ffx` and product bundle are fetched by Bazel, nothing is downloaded while the
test runs. `ffx` state and `HOME` are redirected to a per-run temp directory and the emulator uses
user-mode networking. `/dev/kvm` is required (`requires-kvm` tag).

## Notes

- `fuchsia_remote_product_bundle` of this `rules_fuchsia` snapshot is broken and `ffx product download`
  ignores HTTP proxies, hence the custom repository rule.
- `bazel run` of a component fails in this snapshot (missing python module in `run_component`), and a
  plain component has no log sink in `core/ffx-laboratory`. Hence the program runs as a test.
