# BUILD GUIDE

Use this file only for build and verification entry points.

## Linux fast verification
```bash
cmake -S . -B build-linux-test -G Ninja
cmake --build build-linux-test
ctest --test-dir build-linux-test --output-on-failure
```

## Windows builds (native Windows)
```bash
scripts\\build-win.cmd
scripts\\build-winui.cmd
```

## Windows builds from WSL
```bash
bash scripts/build-win-from-wsl.sh
bash scripts/build-winui-from-wsl.sh
```

## Packaging
```bash
python scripts/package.py --build-dir build-win --out dist/Tullius_ctd_loger.zip --no-pdb
```

## Release gate
```bash
bash scripts/verify_release_gate.sh
```
