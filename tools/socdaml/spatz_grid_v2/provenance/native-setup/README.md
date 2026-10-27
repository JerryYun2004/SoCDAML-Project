# Historical native setup/build provenance

These exact files were copied from the existing native kit during publication
review; [provenance.json](provenance.json) records their source paths and SHA-256.
They retain their original authorship and comments. No script was executed to
install or build anything during capture. [host-dependencies.json](host-dependencies.json)
records installed Python distribution versions and Homebrew opt targets, not a
portable dependency lock.

`native.sh`, `prepare.py`, `snapshot.py`, `toolchain_math.py`, the dependency
probe and Python compatibility constraints preserve the historical **prepare,
dependencies, simulator and toolchain** stages. `env.sh.snapshot` and
`requirements-native.txt` describe the existing workspace; the env snapshot has
historical absolute paths and must not be sourced as portable instructions.

The legacy runner's benchmark/diagnostic branches reference additional historical
kit scripts not included in this setup subset. Do not use those branches: the
repository V2 CLI is the supported execution path for the existing environment.
The archival runner can install dependencies, compile and launch applications;
it is not a read-only checker and is not advertised as a fresh-machine installer.
Its snapshot helper can include dirty/untracked files; publication review instead
uses an isolated committed checkout. See the [sharing guide](../../../../../docs/project/SHARING.md).
